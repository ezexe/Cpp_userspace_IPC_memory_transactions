# Transactions over shared memory, with no kernel on the hot path

C++20 · GCC 13.3 and Clang 18.1 on x86-64 Linux · every snippet is an excerpt of one self-contained header (`utx.hpp`, 350 lines) that was compiled and run; §9 says what ran and what didn't.

## 0. Already checked

- **Atomic against concurrency.** A 4 KiB value written flat out by one process and read by another: zero torn reads in every structure below, GCC and Clang builds.
- **Atomic against death.** Writers were SIGKILLed at random instants, 300 to 400 times per structure. The seqlock left its reader stuck after 231 of 300 kills; the shadow cell after 0 of 300; the ring delivered 1,777,179 frames with no tear, gap or duplicate; the redo-log region kept a 16-account sum at 16,000 through 400 kills, and the same test with in-place writes ended at 15,483.
- **No system call.** Two processes ran all four structures against each other under a seccomp filter that kills on anything but `exit`: 4,000,000 region transactions, both finished. A planted `getpid` got both killed with SIGSYS, so the filter was live.
- **Addresses.** Each child mapped the object itself and got a different address from the parent's.
- **The limit of "pure".** A 5 ms lease with a live owner frozen for 20 ms: 34 thefts from a live process, sum destroyed.
- **Sanitizers.** ThreadSanitizer: 0 reports (it cannot model the fences, see §9). ASan + UBSan: clean.
- **What doesn't cross a process.** GCC's `__transaction_atomic` lost 32,534 to 277,247 of 4,000,000 increments between two processes, under every libitm method; between two threads it lost none.

## 1. Frame

"Pure userspace" here means: once the memory is mapped, no path a transaction takes enters the kernel. No futex, no pipe, no event, no mutex that can park. The mapping itself (`shm_open` + `mmap`, or `CreateFileMappingW` + `MapViewOfFile`) is a kernel act and happens once.

A transaction over that memory has to be atomic against two different things, and most designs give you only the first:

- **Concurrency**: a reader never sees half of a multi-word update. This is the threads problem, unchanged.
- **Death**: a process killed between any two instructions leaves the memory in a state the others can use. Threads don't have this problem, because a dead thread takes the process with it. Processes do: the memory outlives the writer.

Three more axes decide which structure fits:

- **Writers**: one, or several.
- **Shape**: a value that is replaced (state), or records that are appended (a stream).
- **Progress**: wait-free (bounded steps, fit for an audio thread), retrying (a reader may loop), or spinning (waits on another process without being able to sleep).

The result that organises everything below: **with one writer per structure, both atomicities are free, in pure userspace. With several writers, atomicity against death needs someone to say "that owner is dead", and userspace alone can only guess.** §4 measures the guess failing.

## 2. Ground rules, gotchas first

1. **A lock held by a corpse stays held.** Anything that has an in-flight state visible in shared memory (an odd sequence number, a lock word, a "writing" flag) can be frozen in that state for ever. Prefer designs whose only shared transition is one store.
2. **No pointers in the mapping.** Each process has it at its own address (checked: parent and child differed in every run). Indices and offsets only.
3. **`std::atomic<T>` members are the wrong layout type.** `std::atomic<uint64_t>` is not trivially default constructible (checked on GCC 13 and Clang 18), so the process that opens an existing mapping has no rule that brings those objects to life. A struct of plain `uint64_t` is standard-layout and trivial, and `std::atomic_ref` gives it atomic access. This is the form `section.hh` already uses. The blessed way to open one, `std::start_lifetime_as`, is in neither GCC 13.3's nor Clang 18.1's library; until it is, the opener's `static_cast` is what everyone does.
4. **A payload read during a write is a data race in the language, whatever the CPU does.** Optimistic readers must copy through relaxed atomics, not `memcpy`. On x86-64 the relaxed loads are plain moves; a 64-byte read cost 3 to 4 ns.
5. **Zero must be a valid initial state.** A fresh shared mapping is zero-filled. If all-zero means "empty and consistent", there is no initialisation race between creator and opener.
6. **Don't trust what the other side wrote.** Read each index once into a local, validate it, and never read it again. A length the peer controls is an out-of-bounds read otherwise, which matters when the peer is a sandboxed renderer.
7. **You cannot sleep.** A waiter has only `pause`: while the owner is preempted, every waiter burns a core until the owner runs again. Structures where the hot side never waits (A, B, C) don't care; D does.
8. **Software transactional memory is per-process.** libitm's bookkeeping lives in private memory. §6 has the numbers.

The helpers every snippet uses:

```cpp
using u64 = std::uint64_t;
static_assert(std::atomic_ref<u64>::is_always_lock_free);  // lock-free is what makes it address-free

template <class T>
concept Shareable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

inline std::atomic_ref<u64>       at(u64& w) noexcept       { return std::atomic_ref<u64>(w); }
inline std::atomic_ref<const u64> at(const u64& w) noexcept { return std::atomic_ref<const u64>(w); }

template <class T> inline constexpr std::size_t kWordsFor = (sizeof(T) + 7) / 8;

template <Shareable T> void store_words(u64* dst, const T& v) noexcept {
  u64 tmp[kWordsFor<T>]{};
  std::memcpy(tmp, &v, sizeof(T));
  for (std::size_t i = 0; i < kWordsFor<T>; ++i) at(dst[i]).store(tmp[i], std::memory_order_relaxed);
}
template <Shareable T> void load_words(const u64* src, T& out) noexcept {
  u64 tmp[kWordsFor<T>];
  for (std::size_t i = 0; i < kWordsFor<T>; ++i) tmp[i] = at(src[i]).load(std::memory_order_relaxed);
  std::memcpy(&out, tmp, sizeof(T));
}
```

## 3. One writer

### A. Seqlock (`SeqCell`): atomic against concurrency only

**What.** A sequence word, odd while a write is in flight, and the payload. Readers copy optimistically and retry if the sequence moved.

```cpp
template <Shareable T> struct SeqCell {
  alignas(64) u64 seq;
  u64 word[kWordsFor<T>];

  void write(const T& v) noexcept {                       // single writer, wait-free
    const u64 s = at(seq).load(std::memory_order_relaxed);
    at(seq).store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);  // the odd seq is ordered before the payload
    store_words(word, v);
    at(seq).store(s + 2, std::memory_order_release);
  }
  [[nodiscard]] bool try_read(T& out) const noexcept {
    const u64 s1 = at(seq).load(std::memory_order_acquire);
    if (s1 & 1) return false;
    T tmp;
    load_words(word, tmp);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (at(seq).load(std::memory_order_relaxed) != s1) return false;
    out = tmp;
    return true;
  }
};
```

**The fault.** The odd sequence is an in-flight state in shared memory (§2.1). A writer killed inside `write` leaves it odd, and with nobody writing, `try_read` returns false for ever. Measured: 231 of 300 kills left the reader stuck (the payload was 4 KiB, so most of the writer's time was inside the write). A reader cannot repair it: it doesn't know whether the writer is dead or slow, and the payload is half old, half new, so there is nothing to roll to.

**Reach for it** when the writer's death means the reader is torn down too (same lifetime), and memory is tight: it stores the value once. **Skip it** for anything that must survive the writer. B costs one more copy of the value and removes the problem.

### B. Shadow slot (`ShadowCell`): atomic against concurrency and death

**What.** Two slots and a version. The committed value is `slot[ver & 1]`. The writer fills the other slot, which no reader is pointed at, then bumps `ver`. That store is the whole commit.

```cpp
template <Shareable T> struct ShadowCell {
  alignas(64) u64 ver;                 // slot[ver & 1] is the committed value
  u64 slot[2][kWordsFor<T>];

  void write(const T& v) noexcept {    // single writer, wait-free
    const u64 cur = at(ver).load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    store_words(slot[(cur + 1) & 1], v);                 // the shadow
    at(ver).store(cur + 1, std::memory_order_release);   // the commit point
  }
  [[nodiscard]] bool try_read(T& out) const noexcept {
    const u64 v1 = at(ver).load(std::memory_order_acquire);
    T tmp;
    load_words(slot[v1 & 1], tmp);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (at(ver).load(std::memory_order_relaxed) != v1) return false;  // the writer may have reused our slot
    out = tmp;
    return true;
  }
};
```

**Why it survives death.** There is no in-flight state. Killed before the last store, the old value is intact and still published. Killed after, the new one is. A restarted writer reads `ver` and carries on; there is no recovery code because there is nothing to recover. Measured: 0 of 300 kills left the reader stuck or torn, and the value had advanced across all 300.

**Why the reader's check is on `ver` alone.** While `ver == v`, the writer only touches `slot[(v+1) & 1]`. It first touches the reader's slot after committing `v+1`. So an unchanged `ver` proves the copy was not written under.

**Costs.** Twice the memory. Readers retry, and a writer that never pauses can starve them: with the 4 KiB payload written in a tight loop, about 95% of read attempts retried (99% for the seqlock). At a real update rate this vanishes; if it can't, add slots so a reader needs several commits to be lapped.

**Reach for it** as the default for "one process publishes state, others read it": parameters, a transport position, a config block, a header. **Skip it** when the readers must see every update (that is a stream, C), or when several processes write (D).

Uncontended cost for a 64-byte value, both A and B: write 7 to 9 ns, read 3 to 4 ns.

### C. Commit index (`TxRing`): transactions as frames in a single-producer, single-consumer log

**What.** The producer writes any number of records past `w`, then publishes them all with one store of `w`. The consumer acknowledges with one store of `r`.

```cpp
template <std::size_t Cap> requires(std::has_single_bit(Cap) && Cap >= 64)
struct TxRing {
  alignas(64) u64 w;  // producer's; bytes below w are committed
  alignas(64) u64 r;  // consumer's; bytes below r are acknowledged
  alignas(64) std::byte buf[Cap];
  // put(pos, src, n) / get(pos, dst, n): memcpy with wrap at Cap

  class Tx {          // frame = [u64 payload bytes][payload, padded to 8]
   public:
    explicit Tx(TxRing& q) noexcept
        : q_(q), start_(at(q.w).load(std::memory_order_relaxed)), head_(start_ + 8),
          r_(at(q.r).load(std::memory_order_acquire)) {}

    bool append(const void* src, std::size_t n) noexcept {
      if (!ok_) return false;
      if (pad8(head_ + n) - r_ > Cap) {
        r_ = at(q_.r).load(std::memory_order_acquire);
        if (pad8(head_ + n) - r_ > Cap) return ok_ = false;
      }
      q_.put(head_, src, n);
      head_ += n;
      return true;
    }
    bool commit() noexcept {       // not calling it is the abort
      if (!ok_) return false;
      const u64 len = head_ - start_ - 8;
      q_.put(start_, &len, 8);
      at(q_.w).store(pad8(head_), std::memory_order_release);   // the commit point
      return true;
    }
   private:
    TxRing& q_; u64 start_, head_, r_; bool ok_ = true;
  };
};
```

The payload bytes are plain memory, not atomics, and that is correct here: the producer writes only at or above `w`, the consumer reads only below it, and the release store of `w` hands the bytes over. Nothing is ever read while written.

The consumer copies the frame out, then acknowledges:

```cpp
template <class F> Take consume(std::span<std::byte> scratch, F&& f) {
  const u64 rd = at(r).load(std::memory_order_relaxed);
  const u64 wr = at(w).load(std::memory_order_acquire);      // read once, validated, never re-read
  if (rd == wr) return Take::kEmpty;
  const u64 avail = wr - rd;
  if (avail > Cap || avail < 8 || (avail & 7)) return Take::kCorrupt;
  u64 len;
  get(rd, &len, 8);
  if (len > avail - 8) return Take::kCorrupt;
  if (len > scratch.size()) return Take::kTooBig;
  get(rd + 8, scratch.data(), len);
  if (!f(std::span<const std::byte>(scratch.data(), len))) return Take::kKept;
  at(r).store(rd + 8 + pad8(len), std::memory_order_release);  // ack after f, never before
  return Take::kDone;
}
```

**Death, each side.** A producer killed mid-frame never stored `w`, so the frame does not exist; its successor starts at the same `w` and overwrites the debris. A consumer killed inside `f` never stored `r`, so the frame is delivered again: at-least-once, and `f` must tolerate a repeat. Exactly-once needs the effect of `f` and the ack to be one store, which means the consumer's result has to live in the same mapping (a B cell holding both the result and the new `r`).

Measured: 300 producer kills, 1,777,179 three-record frames, none torn, none missing, none duplicated; a refused ack redelivered the same frame once; a frame claiming 2⁶⁴−1 bytes was rejected as `kCorrupt`. Commit plus consume of one 64-byte record: 23 to 25 ns.

**Reach for it** for commands, events, MIDI, PCM blocks: anything where order and completeness matter and there is one producer and one consumer. For several producers, give each its own ring; that keeps every producer wait-free and immune to the others' deaths. **Skip it** for state the reader only wants the latest of (B: no backlog, no full condition).

## 4. Several writers

### D. Redo log + lease lock (`TxRegion`)

**What.** `N` shared words, and transactions that read and write up to `K` of them atomically. Four parts:

- a **lock word** holding the owner's id and a ticket, taken by CAS;
- a **redo log** of `(index, value)` pairs;
- a **`committed` count**, whose store is the commit point;
- a **sequence word**, odd while the log is applied, so readers get consistent snapshots.

```cpp
template <std::size_t N, std::size_t K> struct TxRegion {
  struct Entry { u64 idx, val; };
  alignas(64) u64 lock;     // 0 free, else (owner id << 32) | ticket
  u64 ticket;               // one per acquisition, so a token never repeats for an id
  alignas(64) u64 seq;      // odd while the log is being applied
  u64 committed;            // 0, or the number of valid log entries: THE commit point
  Entry log[K];
  alignas(64) u64 word[N];

  bool replay() noexcept {  // idempotent: entries are absolute values
    const u64 n = at(committed).load(std::memory_order_acquire);
    if (n == 0) return false;
    const u64 s = at(seq).load(std::memory_order_relaxed) | 1;   // already odd if a corpse got that far
    at(seq).store(s, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (u64 i = 0; i < n && i < K; ++i) {
      const u64 idx = at(log[i].idx).load(std::memory_order_relaxed);
      if (idx < N) at(word[idx]).store(at(log[i].val).load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    at(seq).store(s + 1, std::memory_order_release);
    at(committed).store(0, std::memory_order_release);
    return true;
  }
};
```

A transaction buffers its writes in private memory, so an abort has nothing to undo. `commit` copies them to the log, stores `committed`, and then runs `replay`, the same function recovery runs:

```cpp
bool commit() noexcept {
  for (u64 k = 0; k < n_; ++k) {
    at(g_.log[k].idx).store(pend_[k].idx, std::memory_order_relaxed);
    at(g_.log[k].val).store(pend_[k].val, std::memory_order_relaxed);
  }
  at(g_.committed).store(n_, std::memory_order_release);  // the commit point
  g_.replay();
  const bool kept = g_.release(tok_);                     // CAS token -> 0; false = our lease was stolen
  tok_ = 0;
  return kept;
}
```

Every acquisition runs `replay` before doing anything else:

```cpp
template <class Dead> u64 acquire(u32 self, Dead&& dead, bool* stole = nullptr, bool* replayed = nullptr) noexcept {
  for (unsigned spins = 0;; ++spins) {
    u64 cur = at(lock).load(std::memory_order_relaxed);
    const bool free = cur == 0;
    if (free || dead(cur, spins)) {
      const u64 tok = (u64{self} << 32) | (at(ticket).fetch_add(1, std::memory_order_relaxed) & 0xffffffffu);
      if (at(lock).compare_exchange_strong(cur, tok, std::memory_order_acquire, std::memory_order_relaxed)) {
        const bool did = replay();
        if (stole) *stole = !free;
        if (replayed) *replayed = did;
        return tok;
      }
    }
    cpu_relax();
  }
}
```

**Death at every point.** Killed before the `committed` store: the log is garbage nobody reads, the words are untouched, the transaction never happened. Killed after it, anywhere in the apply: the next owner replays the whole log; rewriting words that were already written is harmless because the entries are values, not deltas. Killed after `committed` is cleared: only the lock is stale.

Measured, two writers moving money between 16 accounts and counting commits, a third process killing one of them every millisecond or so: 400 kills, 20,213,231 commits, 88 locks stolen from corpses, 8 half-applied logs replayed, 91,369 reader snapshots all summing to 16,000, final sum 16,000. The same lock and the same thefts with the two words written in place and no log: final sum 15,483.

**The part that is not pure.** `dead(token, spins)` is the verdict that an owner is gone. Two were tested.

The kernel's verdict, asked only on the slow path: `kill(pid, 0) == -1 && errno == ESRCH`. It is right, it costs a system call only while waiting on a held lock, and it has one trap: a killed child that has not been reaped is a zombie and still answers, so its lock is never stolen until the parent calls `waitpid`.

The pure verdict is a lease: the same token, unchanged for longer than `ttl`, is presumed dead.

```cpp
struct Lease {
  std::chrono::nanoseconds ttl;
  u64 token = 0;
  std::chrono::steady_clock::time_point since{};

  bool operator()(u64 cur, unsigned spins) noexcept {
    if (spins % 256 != 0) return false;
    const auto now = std::chrono::steady_clock::now();   // vDSO read: survived the seccomp filter
    if (cur != token) { token = cur; since = now; return false; }
    return now - since > ttl;
  }
};
```

With a 50 ms lease and real kills it works: 60 kills, 15 thefts, sum conserved, and every theft stalls the survivors for the full 50 ms. But a lease cannot tell dead from descheduled. With a 5 ms lease and a live owner frozen by `SIGSTOP` for 20 ms: 34 thefts, all from a live process, which then resumed mid-commit and wrote over its successor. The sum was destroyed. `release` reported every one of the 34 (`lost_lease=34`), but only after the damage.

So the lease is a bet that no owner is ever off-CPU for `ttl` while holding the lock. A debugger breakpoint or a suspended laptop loses the bet. Making `ttl` large makes the bet safer and recovery slower by the same amount.

**Cost.** A 3-word transaction, uncontended: 31 to 33 ns. A robust process-shared `pthread_mutex` around three plain stores: 27 ns. The log is nearly free; what the mutex buys is in §7.

**Reach for it** when several processes must update shared state and none of them is a realtime thread, and you accept either a system call on the recovery path or the lease's bet. **Skip it** on an audio thread (it spins on other processes). Skip it too whenever the state can be split so each piece has one writer: N cells of type B, or a ring per producer, need no lock, no log and no verdict.

## 5. Why several writers can't be made pure

Any multi-writer scheme needs mutual exclusion over the commit, or a CAS-published root with other processes' memory to reclaim. Either way a process can die holding something the others need (a lock, a reference, a reserved slot), and the survivors must decide it is dead in order to take it back. Liveness of another process is a fact only the kernel has. In userspace you can observe that a word hasn't changed, which is equally true of a dead process and a paused one.

Lock-free copy-on-write (allocate a new version, CAS the root offset) moves the problem without removing it: a reader that dies between loading the root and finishing its copy pins that version, and reclaiming it is the same verdict. It also needs tagged offsets against ABA and an allocator in the mapping. I did not build it; the reasoning is the argument above, not a measurement.

## 6. Transactional memory proper

**Hardware (RTM).** RTM is the transactional-memory part of Intel's TSX instruction set. `_xbegin()` starts a transaction, and from there the CPU keeps every write private to the core and remembers which cache lines were read and written. `_xend()` publishes all the writes at once. If another core touches one of those cache lines first, or an interrupt arrives, the CPU throws the writes away and execution resumes at `_xbegin()`, which now returns an abort code instead of `_XBEGIN_STARTED`. Nothing in the kernel is involved in any of this.

It works across processes for the reason the software kind (below) doesn't: the conflict detection is done by the CPU on cache lines of physical memory. Two processes that map the same page conflict exactly as two threads would, and there are no tables or locks in anyone's private memory to get out of step.

The test: `p` points into a shared mapping, and two forked processes each run "add 1 to `p[0]`, subtract 1 from `p[1]`" 2,000,000 times as a hardware transaction. A hardware transaction is allowed to fail for ever, so each attempt is retried up to 8 times and then done under a spinlock kept at `p[2]` in the same mapping. Every transaction reads that lock word first, which makes a process holding the lock abort any transaction running beside it. Measured on this box (CPUID reports RTM): `p[0]` ended at exactly 4,000,000 and `p[1]` at exactly −4,000,000; 3,999,328 of the updates committed in hardware and 672 took the lock.

```cpp
unsigned st = _xbegin();
if (st == _XBEGIN_STARTED) {
  if (__atomic_load_n(&p[2], __ATOMIC_RELAXED)) _xabort(1);   // subscribe to the fallback lock
  p[0]++; p[1]--;
  _xend();
}                                                            // else retry, then take the lock at p[2]
```

Two faults. It always needs a fallback path, and the fallback is a lock in shared memory, which is D with its verdict problem. And you can't count on the instruction set: Intel's microcode from platform update 2021.1 on [disables TSX by default](https://www.intel.com/content/www/us/en/support/articles/000059422/processors.html) on the processors it lists (7th to 9th generation Core among them), and a transaction that always aborts means every commit goes through the fallback. As far as I know AMD has never shipped it; I did not check that. A death inside a hardware transaction is clean (the transaction never commits), which is its one real advantage over D.

**Software (GCC `-fgnu-tm`).** GCC has a language extension for transactional memory: compile with `-fgnu-tm`, wrap a block in `__transaction_atomic { … }`, and its runtime library, libitm, makes the block appear to run all at once with respect to every other such block. It is the obvious thing to try for this document's problem: put the keyword around the updates to the mapping and let the compiler do the rest. It doesn't work across processes, and this is the test that shows it.

`p` points into a shared mapping. Two workers each run this 2,000,000 times:

```cpp
__transaction_atomic { p[0] = p[0] + 1; p[1] = p[1] - 1; }
```

If the transactions are isolated from each other, `p[0]` ends at exactly 4,000,000. Anything less is an increment that one worker overwrote with a stale value. The workers were run once as two threads of one process and once as two forked processes, under each of libitm's implementations (chosen with the `ITM_DEFAULT_METHOD` environment variable). Final `p[0]`:

| libitm method | two threads | two processes |
|---|---|---|
| default | 4,000,000 | 3,911,657 |
| `ml_wt` | 4,000,000 | 3,929,351 |
| `gl_wt` | 4,000,000 | 3,794,223 |
| `serial` | 4,000,000 | 3,722,753 |
| `htm` | 4,000,000 | 3,967,466 |

Between threads it is exact every time. Between processes every method loses increments. The reason is where libitm keeps its bookkeeping: the locks and version counters it uses to detect conflicting transactions are ordinary variables in the process's own private memory. After a fork each process has its own copy, so each one checks only against itself and never learns that the other is in the middle of a transaction on the same words. That includes `htm`, which runs transactions in hardware but falls back to a lock that is private in the same way. In every run `p[1]` ended at exactly minus `p[0]`, so no transaction was ever half-applied; they simply weren't isolated from the other process. The table is the measured part; the explanation is my understanding of libitm's design, and I did not read its source here.

## 7. Cross-compare

| | A `SeqCell` | B `ShadowCell` | C `TxRing` | D `TxRegion` | RTM | Robust mutex (not pure) |
|---|---|---|---|---|---|---|
| Writers | 1 | 1 | 1 producer, 1 consumer | many | many | many |
| Shape | latest value | latest value | ordered frames | N words, K per txn | any | any |
| Atomic vs concurrency | yes | yes | yes | yes | yes | yes |
| Atomic vs writer death | **no** (reader stuck 231/300) | yes (0/300) | yes | yes, given a right verdict | yes | no: you learn of the death, the data is as it was left |
| Commit point | second store of `seq` | store of `ver` | store of `w` | store of `committed` | `_xend` | none |
| Writer progress | wait-free | wait-free | wait-free (fails when full) | spins on other processes | retries, then the fallback | parks in the kernel |
| Reader progress | retries | retries | wait-free | retries | n/a | parks |
| Recovery code | none possible | none needed | none needed | `replay`, the function `commit` already runs | fallback's | yours, after `EOWNERDEAD` |
| Liveness verdict needed | no | no | no | **yes** | for the fallback | kernel supplies it |
| Memory | 1× + 8 B | 2× + 8 B | ring | N + 2K + 4 words, plus padding | 1× | 1× + mutex |
| Audio thread may write | yes | yes | yes | no | no | no |
| Measured, uncontended | 7–9 ns write, 3–4 ns read | same | 23–25 ns commit + consume | 31–33 ns for 3 words | not timed | 27 ns lock + 3 stores + unlock |

The mutex column is there because it is the answer once "pure" is relaxed. A robust `PTHREAD_PROCESS_SHARED` mutex is also userspace when uncontended (hence 27 ns); the kernel is involved only to park waiters and, at the owner's death, to mark the lock so the next `lock` returns `EOWNERDEAD`. That is the correct verdict D lacks. What it doesn't give is the repair: after `EOWNERDEAD` the protected data is half-written, and you still need D's log to finish or discard it. On Windows the equivalent is a named mutex and `WAIT_ABANDONED`, which enters the kernel on every acquisition.

### By direction

- **One process publishes, others observe (state):** B. A only if the writer's death ends the readers too.
- **One process sends, one receives (stream):** C, one ring per direction. This is the shape of the WAX ring, and its "magic published last, with release order" in `Section::Create` is a one-shot commit of the same kind as B's and C's.
- **Both directions between two processes:** two of the above, one per direction. Each side is the single writer of its own structure, so nothing needs a lock.
- **An audio callback that is both, audio and MIDI each way (`processBlock` against a peer):** `duplex-audio-block.md`. Audio becomes position-addressed state, MIDI stays a stream, and the order of the two commits is the block's transaction.
- **Many processes, each with its own state, all reading each other:** one B cell per process.
- **Many processes writing the same words:** D, and the decision is which verdict you can live with.

### Decision guide

1. Can each piece of shared state be given exactly one writing process? Then stop: B for state, C for streams. Both atomicities, wait-free writers, no recovery code, nothing to tune.
2. Does a realtime thread write? Only A, B or C qualify. Use B or C.
3. Must several processes write the same words? Use D. Then: is a system call acceptable while waiting on a held lock? If yes, use the pid verdict (and reap children promptly). If it must stay pure, use a lease, set `ttl` above the longest pause an owner can suffer, and treat `commit()` returning false as corruption.
4. Is "pure" a preference more than a requirement? A robust mutex for the verdict plus D's log for the repair is the sound multi-writer design.
5. Thinking of `__transaction_atomic` or TSX to avoid all this? The first doesn't isolate across processes; the second needs D as its fallback and hardware you can't assume.

## 8. The same thing from TypeScript and C#

The layouts are plain words at fixed offsets, so any language that can do atomic loads and stores on the mapping can take part. `ShadowCell<T>` is `ver` at byte 0 and the two slots from byte 8 (checked with `offsetof`).

```ts
export function shadowWrite(cell: BigUint64Array, value: BigUint64Array): void {
  const cur = Atomics.load(cell, 0);
  const base = 1 + Number((cur + 1n) & 1n) * value.length;
  for (let i = 0; i < value.length; i++) Atomics.store(cell, base + i, value[i]!);
  Atomics.store(cell, 0, cur + 1n); // the commit point
}

export function shadowTryRead(cell: BigUint64Array, out: BigUint64Array): boolean {
  const v1 = Atomics.load(cell, 0);
  const base = 1 + Number(v1 & 1n) * out.length;
  for (let i = 0; i < out.length; i++) out[i] = Atomics.load(cell, base + i);
  return Atomics.load(cell, 0) === v1;
}
```

Run between a worker and the main thread over a `SharedArrayBuffer` with a 4 KiB value: 94,202 reads, 0 torn. `Atomics.load` and `store` are sequentially consistent, which is stronger than the C++ side needs. `Atomics.wait` and `notify` are the futex this document excludes, and as `ring-product-vs-harness.md` records, they don't work on the product's externalized `ArrayBuffer` anyway.

Coming from C#: `MemoryMappedFile` plus `SafeMemoryMappedViewHandle.AcquirePointer` gives the pointer; `Volatile.Read`/`Volatile.Write` on `ref ulong` are the acquire load and release store, and `Interlocked.CompareExchange` is the CAS. A `struct` with `[StructLayout(LayoutKind.Sequential)]` and `ulong` fields matches these layouts once the `alignas(64)` padding is written out as explicit fields. .NET's own `Mutex` is the Windows named mutex of §7, with `AbandonedMutexException` as its `EOWNERDEAD`. None of the C# was compiled.

## 9. Verification status

**Compiled.** `utx.hpp` and its test: `g++ 13.3 -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Wpedantic`, zero diagnostics; `clang++ 18.1.3 -std=c++20 -O2 -Wall -Wextra`, zero diagnostics. The TypeScript: `tsc 7.0.2 --strict --noUncheckedIndexedAccess`, then run on Node 22.

**Run, x86-64 Linux, 2 vCPUs.** Every count in this document is from a single run of each test, and the timings are the range over three runs; repeat runs of the kill tests differed in counts and never in the zero/non-zero pattern. Processes were forked, each child mapped the `shm_open` object itself, and kills were `SIGKILL` at uniformly random delays of 0.3 to 1.8 ms.

- Cells: 1.5 s of concurrent reading, then 300 kills each. Clang build gave the same pattern (seqlock stuck 218/300, shadow 0/300).
- Ring: 300 producer kills, sequence checked frame by frame; refused ack; hostile length.
- Region: four configurations, 3 to 8 s each, as quoted in §4.
- Purity: seccomp filter allowing only `exit`, installed after mapping; 2,000,000 iterations per child of every structure; the lease read the clock 957,787 times under contention without being killed. Negative control with a planted `getpid`: both children died of SIGSYS.
- ThreadSanitizer (threads in one process, all four structures): 0 reports. GCC warns that `atomic_thread_fence` is not supported under TSan, so this run says nothing about whether the fences are sufficient; every shared access is an atomic operation, so there was nothing else for it to flag.
- ASan + UBSan: shadow cell, ring, and the redo-log region test, clean.
- RTM and libitm: as tabulated in §6.
- Layout: `offsetof` and the type traits quoted in §2.3 and §8, printed by a compiled program.

**Found on the way.** Under `SECCOMP_MODE_STRICT` the lease's first clock read died with SIGSEGV. Strict mode also disables `RDTSC`, which the vDSO clock uses; I'm inferring that as the cause from the signal, and the purity test uses filter mode because of it.

**Not verified.**

- Anything on Windows or MSVC 19.51: the header uses only `<atomic>`, `<bit>`, `<chrono>`, `<span>` and `_mm_pause`, but it was not compiled there, and `steady_clock` staying in userspace on Windows is unchecked.
- ARM64 or any weakly ordered CPU. The fences follow the C++ memory model's rules for seqlocks, but x86's ordering would hide a missing one and TSan can't see them.
- More than two writers on the region, and more than 2 cores.
- The copy-on-write alternative in §5 (argued, not built).
- AMD's lack of TSX, and RTM on any machine but this one. The Intel statement is read from Intel's support article, not tested.
- The C# mapping in §8.
- A cross-language run (C++ writer, JS reader on one mapping): the TS test shares the layout, not the memory, with the C++.
