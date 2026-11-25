// utx.hpp: transactions over shared memory with no kernel call on the hot path.
// Self-contained, C++20. Every type is a plain standard-layout struct of uint64_t and bytes, so the
// creator's zero-filled mapping is a valid initial state and the layout can be read from any language.
#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace utx {

using u64 = std::uint64_t;
using u32 = std::uint32_t;

// A lock-free atomic is address-free, which is what makes it usable from two mappings of one page.
static_assert(std::atomic_ref<u64>::is_always_lock_free);
static_assert(std::atomic_ref<u64>::required_alignment == alignof(u64));

// No pointers, no vtables, no std:: containers: only bytes that mean the same at any address.
template <class T>
concept Shareable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

inline std::atomic_ref<u64> at(u64& w) noexcept { return std::atomic_ref<u64>(w); }
inline std::atomic_ref<const u64> at(const u64& w) noexcept { return std::atomic_ref<const u64>(w); }  // load only

inline void cpu_relax() noexcept {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("isb");
#endif
}

template <class T>
inline constexpr std::size_t kWordsFor = (sizeof(T) + 7) / 8;

// Payload words are read while they may be written, so they go through relaxed atomics: a plain
// memcpy there is a data race in the language's terms, whatever the hardware does.
template <Shareable T>
inline void store_words(u64* dst, const T& v) noexcept {
  u64 tmp[kWordsFor<T>]{};
  std::memcpy(tmp, &v, sizeof(T));
  for (std::size_t i = 0; i < kWordsFor<T>; ++i) { at(dst[i]).store(tmp[i], std::memory_order_relaxed); }
}
template <Shareable T>
inline void load_words(const u64* src, T& out) noexcept {
  u64 tmp[kWordsFor<T>];
  for (std::size_t i = 0; i < kWordsFor<T>; ++i) { tmp[i] = at(src[i]).load(std::memory_order_relaxed); }
  std::memcpy(&out, tmp, sizeof(T));
}

// ---------------------------------------------------------------------------------------------
// A. SeqCell: one writer, any readers. Atomic against concurrency, NOT against the writer's death.
// ---------------------------------------------------------------------------------------------
template <Shareable T>
struct SeqCell {
  alignas(64) u64 seq;  // odd while a write is in flight
  u64 word[kWordsFor<T>];

  // Single writer. Wait-free.
  void write(const T& v) noexcept {
    const u64 s = at(seq).load(std::memory_order_relaxed);
    at(seq).store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);  // orders the odd seq before the payload
    store_words(word, v);
    at(seq).store(s + 2, std::memory_order_release);
  }

  // False means "in flight or changed under me": retry, or give up; never block.
  [[nodiscard]] bool try_read(T& out) const noexcept {
    const u64 s1 = at(seq).load(std::memory_order_acquire);
    if (s1 & 1) { return false; }
    T tmp;
    load_words(word, tmp);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (at(seq).load(std::memory_order_relaxed) != s1) { return false; }
    out = tmp;
    return true;
  }

  // A writer that died mid-write leaves seq odd for ever; only a new writer can know and repair it.
  [[nodiscard]] bool torn() const noexcept { return at(seq).load(std::memory_order_acquire) & 1; }
};

// ---------------------------------------------------------------------------------------------
// B. ShadowCell: one writer, any readers. Atomic against concurrency AND the writer's death:
//    the commit is one store, and there is no in-flight state for a corpse to leave behind.
// ---------------------------------------------------------------------------------------------
template <Shareable T>
struct ShadowCell {
  alignas(64) u64 ver;  // slot[ver & 1] is the committed value
  u64 slot[2][kWordsFor<T>];

  // Single writer. Wait-free. A restarted writer needs no recovery: it reads ver and carries on.
  void write(const T& v) noexcept {
    const u64 cur = at(ver).load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    store_words(slot[(cur + 1) & 1], v);                   // the shadow: no reader is pointed at it
    at(ver).store(cur + 1, std::memory_order_release);     // the commit point
  }

  [[nodiscard]] bool try_read(T& out) const noexcept {
    const u64 v1 = at(ver).load(std::memory_order_acquire);
    T tmp;
    load_words(slot[v1 & 1], tmp);
    std::atomic_thread_fence(std::memory_order_acquire);
    // Unchanged ver: the writer has not yet been allowed to touch the slot we read.
    if (at(ver).load(std::memory_order_relaxed) != v1) { return false; }
    out = tmp;
    return true;
  }
};

// ---------------------------------------------------------------------------------------------
// C. TxRing: one producer, one consumer. A transaction is a frame of any number of records; it
//    becomes visible in one store of w. Unpublished bytes do not exist as far as the consumer goes.
// ---------------------------------------------------------------------------------------------
template <std::size_t Cap>
  requires(std::has_single_bit(Cap) && Cap >= 64)
struct TxRing {
  alignas(64) u64 w;  // producer's; bytes below w are committed
  alignas(64) u64 r;  // consumer's; bytes below r are acknowledged
  alignas(64) std::byte buf[Cap];

  static constexpr u64 pad8(u64 n) noexcept { return (n + 7) & ~u64{7}; }

  void put(u64 pos, const void* src, std::size_t n) noexcept {
    const std::size_t o = pos & (Cap - 1), first = n < Cap - o ? n : Cap - o;
    std::memcpy(buf + o, src, first);
    std::memcpy(buf, static_cast<const std::byte*>(src) + first, n - first);
  }
  void get(u64 pos, void* dst, std::size_t n) const noexcept {
    const std::size_t o = pos & (Cap - 1), first = n < Cap - o ? n : Cap - o;
    std::memcpy(dst, buf + o, first);
    std::memcpy(static_cast<std::byte*>(dst) + first, buf, n - first);
  }

  // Producer side. Frame = [u64 payload bytes][payload, padded to 8].
  class Tx {
   public:
    explicit Tx(TxRing& q) noexcept
        : q_(q), start_(at(q.w).load(std::memory_order_relaxed)), head_(start_ + 8),
          r_(at(q.r).load(std::memory_order_acquire)) {}

    // Appends one record to the uncommitted frame. False (and the Tx is poisoned) when it won't fit.
    bool append(const void* src, std::size_t n) noexcept {
      if (!ok_) { return false; }
      if (pad8(head_ + n) - r_ > Cap) {
        r_ = at(q_.r).load(std::memory_order_acquire);
        if (pad8(head_ + n) - r_ > Cap) { return ok_ = false; }
      }
      q_.put(head_, src, n);
      head_ += n;
      return true;
    }
    template <Shareable R>
    bool append(const R& rec) noexcept { return append(&rec, sizeof(R)); }

    // The commit point. Not calling it is the abort: the next Tx starts at the same w.
    bool commit() noexcept {
      if (!ok_) { return false; }
      const u64 len = head_ - start_ - 8;
      q_.put(start_, &len, 8);
      at(q_.w).store(pad8(head_), std::memory_order_release);
      return true;
    }

   private:
    TxRing& q_;
    u64 start_, head_, r_;
    bool ok_ = true;
  };

  enum class Take { kEmpty, kDone, kKept, kCorrupt, kTooBig };

  // Consumer side. Copies one frame into `scratch` and calls f(span) -> bool. The frame is
  // acknowledged only if f returns true, and only after it returns: a consumer that dies inside f
  // sees the same frame again (at-least-once). The indices are read once and validated; the
  // producer is not trusted to have written a sane length.
  template <class F>
  Take consume(std::span<std::byte> scratch, F&& f) {
    const u64 rd = at(r).load(std::memory_order_relaxed);
    const u64 wr = at(w).load(std::memory_order_acquire);
    if (rd == wr) { return Take::kEmpty; }
    const u64 avail = wr - rd;
    if (avail > Cap || avail < 8 || (avail & 7)) { return Take::kCorrupt; }
    u64 len;
    get(rd, &len, 8);
    if (len > avail - 8) { return Take::kCorrupt; }
    if (len > scratch.size()) { return Take::kTooBig; }
    get(rd + 8, scratch.data(), len);
    if (!f(std::span<const std::byte>(scratch.data(), len))) { return Take::kKept; }
    at(r).store(rd + 8 + pad8(len), std::memory_order_release);
    return Take::kDone;
  }
};

// ---------------------------------------------------------------------------------------------
// D. TxRegion: many writers, any readers, N words, up to K words per transaction.
//    Redo log + lease lock + seqlock. Atomic against concurrency and against a writer's death at
//    any instruction, provided the lease's verdict ("that owner is dead") is right.
// ---------------------------------------------------------------------------------------------

// The pure-userspace verdict: the same lock token, unchanged for longer than ttl, is a corpse.
// It cannot tell a dead owner from a descheduled one. ttl is the only knob.
struct Lease {
  std::chrono::nanoseconds ttl;
  u64 token = 0;
  std::chrono::steady_clock::time_point since{};

  bool operator()(u64 cur, unsigned spins) noexcept {
    if (spins % 256 != 0) { return false; }  // the clock is a vDSO/QPC read, not a syscall; still not free
    const auto now = std::chrono::steady_clock::now();
    if (cur != token) {
      token = cur;
      since = now;
      return false;
    }
    return now - since > ttl;
  }
};

template <std::size_t N, std::size_t K>
struct TxRegion {
  struct Entry {
    u64 idx, val;
  };

  alignas(64) u64 lock;    // 0 free, else (owner id << 32) | ticket
  u64 ticket;              // one per acquisition, so a token never repeats for an id
  alignas(64) u64 seq;     // odd while the log is being applied to word[]
  u64 committed;           // 0, or the number of valid log entries: THE commit point
  Entry log[K];
  alignas(64) u64 word[N];

  // Idempotent. Runs after every acquisition; a no-op unless the previous owner died after its
  // commit point. Entries are absolute values, so replaying a half-applied log is harmless.
  bool replay() noexcept {
    const u64 n = at(committed).load(std::memory_order_acquire);
    if (n == 0) { return false; }
    const u64 s = at(seq).load(std::memory_order_relaxed) | 1;  // already odd if the corpse got that far
    at(seq).store(s, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (u64 i = 0; i < n && i < K; ++i) {
      const u64 idx = at(log[i].idx).load(std::memory_order_relaxed);
      if (idx < N) { at(word[idx]).store(at(log[i].val).load(std::memory_order_relaxed), std::memory_order_relaxed); }
    }
    at(seq).store(s + 1, std::memory_order_release);
    at(committed).store(0, std::memory_order_release);
    return true;
  }

  // Spins; never parks. `dead(token, spins)` decides when a held lock may be stolen.
  // Returns the token; *stole / *replayed report what the acquisition found.
  template <class Dead>
  u64 acquire(u32 self, Dead&& dead, bool* stole = nullptr, bool* replayed = nullptr) noexcept {
    for (unsigned spins = 0;; ++spins) {
      u64 cur = at(lock).load(std::memory_order_relaxed);
      const bool free = cur == 0;
      if (free || dead(cur, spins)) {
        const u64 tok = (u64{self} << 32) | (at(ticket).fetch_add(1, std::memory_order_relaxed) & 0xffffffffu);
        if (at(lock).compare_exchange_strong(cur, tok, std::memory_order_acquire, std::memory_order_relaxed)) {
          const bool did = replay();
          if (stole) { *stole = !free; }
          if (replayed) { *replayed = did; }
          return tok;
        }
      }
      cpu_relax();
    }
  }

  // False: the lock was no longer ours, i.e. somebody judged us dead while we were alive.
  bool release(u64 tok) noexcept {
    return at(lock).compare_exchange_strong(tok, 0, std::memory_order_release, std::memory_order_relaxed);
  }

  // Writes are buffered here, in private memory. Nothing shared changes before commit().
  class Tx {
   public:
    template <class Dead>
    Tx(TxRegion& g, u32 self, Dead&& dead, bool* stole = nullptr, bool* replayed = nullptr) noexcept
        : g_(g), tok_(g.acquire(self, dead, stole, replayed)) {}
    Tx(const Tx&)            = delete;
    Tx& operator=(const Tx&) = delete;
    ~Tx() {
      if (tok_) { g_.release(tok_); }  // abort: there is nothing to undo
    }

    [[nodiscard]] u64 read(std::size_t i) const noexcept {
      for (u64 k = n_; k-- > 0;) {
        if (pend_[k].idx == i) { return pend_[k].val; }
      }
      return at(g_.word[i]).load(std::memory_order_relaxed);  // stable: we hold the lock
    }
    bool write(std::size_t i, u64 v) noexcept {
      for (u64 k = 0; k < n_; ++k) {
        if (pend_[k].idx == i) { return pend_[k].val = v, true; }
      }
      if (n_ == K || i >= N) { return false; }
      pend_[n_++] = {i, v};
      return true;
    }

    // True: applied. False: the lease was lost before we released (see release()).
    bool commit() noexcept {
      for (u64 k = 0; k < n_; ++k) {
        at(g_.log[k].idx).store(pend_[k].idx, std::memory_order_relaxed);
        at(g_.log[k].val).store(pend_[k].val, std::memory_order_relaxed);
      }
      at(g_.committed).store(n_, std::memory_order_release);  // the commit point
      g_.replay();                                            // the owner's apply is the same code as recovery
      const bool kept = g_.release(tok_);
      tok_ = 0;
      return kept;
    }

   private:
    TxRegion& g_;
    u64 tok_;
    u64 n_ = 0;
    Entry pend_[K];
  };

  // A consistent copy of all N words, or false. Stuck false after a writer died mid-apply, until
  // anyone acquires the lock (recover() below does only that).
  [[nodiscard]] bool try_snapshot(u64 (&out)[N]) const noexcept {
    const u64 s1 = at(seq).load(std::memory_order_acquire);
    if (s1 & 1) { return false; }
    for (std::size_t i = 0; i < N; ++i) { out[i] = at(word[i]).load(std::memory_order_relaxed); }
    std::atomic_thread_fence(std::memory_order_acquire);
    return at(seq).load(std::memory_order_relaxed) == s1;
  }

  template <class Dead>
  void recover(u32 self, Dead&& dead) noexcept { release(acquire(self, dead)); }
};

}  // namespace utx
