# The duplex audio callback: one `processBlock`, producer and consumer of audio and MIDI

C++20 · GCC 13.3 and Clang 18.1 on x86-64 Linux · JUCE 9.0.6's `AudioBuffer` and `MidiBuffer` · the companion to `userspace-ipc-transactions.md`, whose structures this builds on. Every snippet is an excerpt of one self-contained header (`duplex.hpp`, 407 lines, on top of that doc's `utx.hpp`) that was compiled and run; §9 says how.

## 0. Already checked

- **Kept in step through faults.** The host was paced like a DAW at 48 kHz, with block sizes 1 to 512 at random. The peer rendered 128-frame quanta in another process. Every output frame encodes the input position it came from. Across 15 configurations, including 22 peer kills and 20 freezes of 20 ms each, no frame was misaligned or torn. A fault cost silence, never a shift.
- **Never waits on the peer.** Decoupled, the host's longest callback was 21.9 to 39.2 µs in the fault runs, whether the peer was killed or frozen. Healthy runs had single outliers up to 151 µs (and once 3.7 ms), from a scheduler that gives this container no realtime priority. The same host spinning on the peer under a 1 ms budget (lockstep) spent the whole budget 71 times while the peer was frozen.
- **Commit order is the cross-ring transaction.** The host was stalled 200 µs between its two commits. With MIDI committed before audio, 0 events arrived after their audio. With audio first, 588 did.
- **MIDI is late, not lost.** With the latency too short, or the peer frozen, 0 events were lost; they were delivered late instead. With the peer killed, the events lost were exactly the ones its death took with it: 14 queued for it plus 15 sent by the dead peer, 29 in all.
- **Dead peers leave stuck notes unless you end them.** With the generation check, 0 notes were held past their note-off. Without it, 5 were, one for 96,564 frames, until the same note was struck again.
- **The FIFO alternative drifts.** A cursor-addressed FIFO pair started at 589 frames of latency and ended at 628 to 1,494, and stayed there.
- **No syscalls on the hot path.** The host ran 743 blocks under a seccomp filter that kills on any system call but `exit`, in both modes. A planted `getpid` got it killed with SIGSYS.
- **JUCE.** Against JUCE's own buffers, the glue handled 20,000 blocks, including 230 of zero samples and 195 of up to four times the prepared size, with 0 misaligned frames. `MidiBuffer` allocated only when it reached a new maximum, with or without `swapWith`. One `ensureSize` on the host's buffer in the first block brought that to 0 for the remaining 19,999 blocks.

## 1. Frame

`userspace-ipc-transactions.md` settled that one writer per structure gives atomicity against both concurrency and death, with nothing to recover. The duplex callback keeps that. What changes is that one call on one thread is the producer of two channels and the consumer of two others, against a peer that runs on its own schedule:

| Channel | Writer | Reader | Loss means |
|---|---|---|---|
| `in` audio | host's `processBlock` | peer | a dropout |
| `midi_in` | host's `processBlock` | peer | a stuck or missing note |
| `out` audio | peer | host's `processBlock` | a dropout |
| `midi_out` | peer | host's `processBlock` | a stuck or missing note |

Four structures, each with one writer, so the first doc's rules hold for each. Three decisions are left, and they're the axes of everything below:

- **Addressing.** Do readers ask for "the next frames" (a cursor, a FIFO) or for "the frames at position *p*" (the host's sample clock)? This decides what a stall does to latency.
- **Pacing.** Does the host wait inside the callback for the peer to answer the block it just sent (lockstep), or read an answer from a block it sent earlier (decoupled)? This trades latency against isolation.
- **Loss semantics.** Audio is state per position: once its position has played, a late frame is worthless and a lost one is a dropout. MIDI is a stream: a late note-off still matters, and a lost one is a note that never ends. So the two need different structures, even in the same block.

## 2. Gotchas first

1. **The buffer is in place.** JUCE's `processBlock` contract: the buffer arrives holding input and must leave holding output. With more outputs than inputs, the extra channels may hold garbage; with more inputs than outputs, the extra input channels may be read-only host memory. So the host copies its input into `in` before it writes a single output frame: `send`, then `receive`. `receive` clears output channels the ring doesn't carry.
2. **Block size is not fixed, has no upper bound, and can be zero.** The same contract says a block may be larger or smaller than the size given to `prepareToPlay`, and that some hosts pass zero samples. `send` and `receive` therefore take any `n` and move it in writes of at most `M` frames, the ring's bound. Measured with blocks up to `4M` and the latency sized for `M`: 0 misaligned frames. The frames the latency doesn't cover came out silent, and their MIDI was delivered late (355 events, 0 lost).
3. **The peer's quantum isn't the host's block.** Web Audio renders 128 frames at a time; the host hands over whatever it likes. Blocks never line up; positions do. Every frame and every event carries its absolute position on the host's clock, and each side works in its own unit.
4. **A FIFO's cursor counts frames it accepted, not time.** If the peer stalls, the FIFO holds a backlog. When the peer resumes and drains it at the same rate the host consumes, the backlog becomes permanent latency. Measured in §3.A.
5. **Two rings commit in an order, and the order is the transaction.** The writer commits MIDI before audio; the reader reads audio before MIDI. Then a reader that sees the audio for a position also sees that position's events, without any combined commit. Get it backwards and events miss their block whenever the writer is preempted between the two stores (§4).
6. **A dead peer's note-ons have no note-offs.** The process that would have sent them is gone. The new peer can't send them either: it starts fresh, and drops the MIDI addressed to the past. Only the host knows what it delivered, so the host ends those notes when the peer's generation changes (§5).
7. **A peer that falls behind must not drop MIDI.** When it jumps ahead to catch up, the events for the frames it skipped are note-offs as often as note-ons. It delivers them late, at offset 0 of its next quantum; it drops past MIDI only when it is a new process.
8. **`MidiBuffer` allocates when it grows, and the host owns the buffer that grows.** In JUCE 9.0.6, `addEvent` goes through `Array::insertMultiple` to `ensureAllocatedSize`, which reallocates when full. `clear()` keeps the capacity, `ensureSize` preallocates, and `swapWith` swaps storage without allocating. The common advice (build into a preallocated member, then `swapWith`) doesn't help: the swap hands the host's smaller storage back to you, and it grows on a later block. Measured in §6.
9. **The clock lives in the mapping.** `prepareToPlay` can run again while the peer is alive. If the host restarted its clock at zero, positions would go backwards. `prepare()` resumes from `in.w`.
10. **Don't trust what the peer wrote.** The host clamps the peer's `w` to its own input clock (the peer can't have rendered audio it was never sent). It checks every event's size, and empties a corrupt MIDI lane rather than reading it.
11. **Never two peers at once.** A new peer process may write `out` only after the old one is dead and reaped. Two live writers are the multi-writer problem from the first doc, and only the kernel knows which one is dead. In CEF the browser process learns this from the render-process-terminated callback; in the test, `waitpid` returns.

## 3. Options

### A. FIFO pair: cursor-addressed (the shape of WAX's `Inlet`/`Outlet`)

**What.** Two backpressured SPSC rings with read and write cursors. The host appends what fits to the input ring and reads whatever is in the output ring, filling with silence when short. The peer moves frames from one to the other a quantum at a time. Payload copies can be plain `memcpy`, because each side only touches bytes the other has handed over.

**The fault.** Latency is whatever has accumulated between the two cursors, and nothing brings it back down. Measured, same host and peer as everywhere else, with latency read from the output values:

| Run | Median latency, 0.5–1.5 s | Median latency, last second |
|---|---|---|
| no faults | 589 frames | 628 / 1,221 (two runs) |
| peer frozen 20 ms, 20 times | 589 frames | 1,353 / 1,421 / 1,494 (three runs) |

The rise without faults came with 641 frames of silence in that run: ordinary scheduling hiccups on this two-core box. The amount ratchets to the largest stall seen, not to the sum of the stalls: the backlog from the first stall absorbs the later ones. Each run ended between 7% and 154% above where it started. A host's delay compensation can't follow that, because it is reported once.

**Reach for it** when nothing downstream cares about a fixed latency (a meter, a scope), or with lockstep pacing that stops a backlog from forming. **Skip it** for audio the host plays: §3.B costs one comparison more per read and never drifts.

### B. Position-addressed, decoupled: the host never waits

**What.** Each audio direction is a `SampleRing`: one writer, readers addressed by position, every slot rewritten once per lap, and validation after the copy. It's the shadow cell of the first doc spread over positions. The writer never looks at the reader, so neither side has a cursor the other can stall.

```cpp
template <std::size_t Ch, std::size_t S, std::size_t M>
  requires(std::has_single_bit(S) && M > 0 && 2 * M <= S)
struct SampleRing {
  alignas(64) u64 w;  // positions below w are committed
  u64 base;           // positions below base were never written by the current run
  u64 gen;            // the writer process's generation; bumped by each new writer
  alignas(64) u32 bits[Ch][S];

  void write(const float* const* src, std::size_t channels, std::size_t n) noexcept {  // n <= M
    const u64 p = at(w).load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);  // a reader that sees these bits sees w >= p
    for (std::size_t c = 0; c < Ch; ++c) {
      const float* s = c < channels ? src[c] : nullptr;
      for (std::size_t i = 0; i < n; ++i) {
        std::atomic_ref<u32>(bits[c][(p + i) & kMask])
            .store(s != nullptr ? std::bit_cast<u32>(s[i]) : 0u, std::memory_order_relaxed);
      }
    }
    at(w).store(p + n, std::memory_order_release);
  }

  void rebase(u64 pos) noexcept {  // the run moves on; skipped positions hold an older lap
    pos = std::max(pos, at(w).load(std::memory_order_relaxed));
    at(base).store(pos, std::memory_order_relaxed);
    at(w).store(pos, std::memory_order_release);
  }
};
```

The read copies, then looks at `w` again. It is the seqlock reader with the sequence number replaced by the write position:

```cpp
Span read(u64 t, float* const* dst, std::size_t channels, std::size_t n, u64 limit = ~u64{0}) const noexcept {
  const u64 w1 = std::min(at(w).load(std::memory_order_acquire), limit);
  const u64 b  = at(base).load(std::memory_order_relaxed);
  u64 lo = std::max(t, b), hi = std::min(t + n, w1);
  if (lo < hi) {
    for (std::size_t c = 0; c < channels && c < Ch; ++c)
      for (u64 q = lo; q < hi; ++q)
        dst[c][q - t] = std::bit_cast<float>(std::atomic_ref<const u32>(bits[c][q & kMask]).load(std::memory_order_relaxed));
    std::atomic_thread_fence(std::memory_order_acquire);
    // The writer may be rewriting the slots of [w2 + M - S, w2 + M - S + M) right now, and has rewritten
    // everything below that already. What we copied from there is suspect.
    const u64 w2     = at(w).load(std::memory_order_relaxed);
    const u64 floor2 = w2 + M > S ? w2 + M - S : 0;
    lo               = std::clamp(floor2, lo, hi);
  } else {
    lo = hi = t;
  }
  // ... silence outside [lo, hi), then:
  return {lo, hi};
}
```

**Why the second look is enough.** A writer that has published `w2` can be writing at most `M` positions past it. Those land in the slots of `[w2 + M - S, w2 + M - S + M)`, and everything below that has already been overwritten. The writer's release fence comes after its previous store of `w`. So if the copy saw any byte from a later block, the reader's load of `w` after its acquire fence sees at least that block's start, and the range check moves `lo` past the suspect positions. Measured at the edge of the lap (a reader asking for the oldest range the rule allows, against a writer running flat out): 0 wrong frames in 60.4 M accepted. The same copy without the second look: 3.6 M wrong.

**Why `base`.** A writer that restarts further on (a new peer, or one catching up) leaves slots in the gap holding the previous lap's audio. Those positions are below the new `w`, so `w` alone would accept them. Measured with a 512-frame gap: with `rebase`, 512 frames silent and 0 stale. With a bare store of `w`, all 512 were the old lap, played as if current.

**MIDI is a stream**, so it gets the first doc's `TxRing`, one event per frame. Each event carries its position and its writer's generation:

```cpp
struct MidiEvent {
  u64 pos;      // absolute, on the host's clock
  u32 gen;      // the writing peer's generation (0 from the host)
  u8 size;      // 1..3
  u8 data[3];
};
```

`take_before(end, max, f)` hands over, in order, each event stamped before `end`, acknowledging it after `f` returns. An event that is late still comes out, marked late, at offset 0. Only a full lane drops one.

**The host's half**, in the order the gotchas require:

```cpp
template <class Events>
void send(const float* const* io, std::size_t channels, std::size_t n, const Events& events) noexcept {
  const u64 at_pos = at(d_.in.w).load(std::memory_order_relaxed);  // ours: the host is in's only writer
  for (const BlockEvent& e : events) {                              // MIDI first...
    MidiEvent m{at_pos + e.offset, 0, e.size, {e.data[0], e.data[1], e.data[2]}};
    if (!d_.midi_in.push(m)) { ++r_.midi_in_dropped; }
  }
  channels = std::min(channels, D::kChannels);
  const float* part[D::kChannels];
  for (std::size_t off = 0; off < n; off += D::kMaxBlock) {          // ...then audio, in writes of <= M
    for (std::size_t c = 0; c < channels; ++c) { part[c] = io[c] + off; }
    d_.in.write(part, channels, std::min(n - off, D::kMaxBlock));
  }
}
```

`receive` reads the peer's audio for `[clock - L, +n)`, clamped to what the host has sent. It then checks the peer's generation, then takes the MIDI:

```cpp
const Span s = /* d_.out.read(from + off, ...) per chunk of <= M */;
const u64 g = at(d_.out.gen).load(std::memory_order_acquire);
if (g != gen_) { regenerate(g, from, emit); }  // end the notes the dead peer started
d_.midi_out.take_before(from + n, ~std::size_t{0}, [&](const MidiEvent& e) {
  if (e.gen < gen_) { ++r_.midi_out_stale; return; }  // a dead peer's: its note-ons would never end
  if (e.gen > gen_) { regenerate(e.gen, from, emit); }
  if (e.size < 1 || e.size > 3) { ++r_.midi_out_bad; return; }
  const bool late = e.pos < from;
  track(e.data[0], e.data[1], e.size > 2 ? e.data[2] : u8{0});
  emit(Delivered{e.pos, late ? 0u : static_cast<u32>(e.pos - from),
                 late ? Delivered::kLate : Delivered::kOnTime, e.size, {e.data[0], e.data[1], e.data[2]}});
});
```

**The peer's half** renders one quantum when its input is committed. It jumps forward if it has been lapped, and commits in the same order as the host:

```cpp
template <class Render>
Step step(Render&& render) {
  const u64 wi = at(d_.in.w).load(std::memory_order_acquire);
  if (wi < p_ + Q) { return Step::kNotYet; }
  // ... d_.in.read(p_, inp, Ch, Q) -> s
  if (s.lo != p_ || s.hi != p_ + Q) { jump(wi); ++r_.jumps; return Step::kJumped; }  // lapped
  // ... take MIDI before p_ + Q (late events at offset 0), render(in, out, events, count, emit)
  d_.out.write(outp, D::kChannels, Q);  // after the MIDI emit pushed: the host reads audio first
  p_ += Q;
  return Step::kRendered;
}

void start() noexcept {  // a new peer process; the old one is dead and reaped
  gen_ = at(d_.out.gen).load(std::memory_order_relaxed) + 1;
  at(d_.out.gen).store(gen_, std::memory_order_release);
  jump(at(d_.in.w).load(std::memory_order_acquire));
  // The input clock was read first, so nothing at or after the new position is dropped.
  d_.midi_in.take_before(p_, ~std::size_t{0}, [&](const MidiEvent&) { ++r_.midi_in_dropped_at_start; });
}

void jump(u64 pos) noexcept {  // MIDI is kept: the skipped positions' note-offs arrive late
  p_ = std::max(pos / Q * Q, at(d_.out.w).load(std::memory_order_relaxed));
  d_.out.rebase(p_);
}
```

**The latency it needs.** At the start of a block at `t`, the peer can have rendered only whole quanta of input committed by earlier callbacks: up to `floor(t / Q) * Q`, and only if it has had time to run. The block reads `[t - L, t - L + n)`. Take the worst case, `n = M` with `t` one frame short of a quantum boundary:

> **L ≥ M + Q − 1 + R × rate**, where R is how long the peer may take to react.

With `M = 512` and `Q = 128` that is 639 frames (13.3 ms at 48 kHz) plus the margin. Report it to the host with `setLatencySamples(L)`, so delay compensation lines up the other tracks. Measured, with the peer polling every 50 µs:

| L | Silent frames after the first 0.5 s | MIDI of 761–762 events |
|---|---|---|
| 127 (Q − 1) | 278,785 of 384k | 184 on time, 577 late, 0 lost |
| 639 (M + Q − 1) | 0, 0, 932 (three runs) | all on time, or 2 late |
| 735 (+ 2 ms) | 0, 0, 2, 5 (four runs) | all on time |

At 639 the formula leaves R = 0, and a hiccup in the peer's scheduling shows through. At 735 what's left is this container having no realtime scheduling. On a machine where the peer's audio thread is realtime, R is the peer's own wake-up jitter.

**Under faults**, at L = 735:

| Fault | Misaligned | Silent after 0.5 s | MIDI | Host callback max |
|---|---|---|---|---|
| peer SIGKILLed 22× (dead 0–20 ms each) | 0 | 15,529 | 29 lost = 14 dropped at start + 15 stale; 8 notes ended by the host; 0 held past due | 38.7 µs |
| same, without the generation logic | 0 | 15,298 | 18 lost (all dropped at start); **5 notes held past due, one for 96,564 frames** | 21.9 µs |
| peer SIGSTOPped 20 ms, 20× | 0 | 8,234 (about 410 per freeze: 960 − 735 + catch-up) | 0 lost, 13 late | 39.2 µs |

After every fault the output came back at exactly L, because the frame at `t − L` is either right or silent. Silence per freeze is the freeze minus the latency, so a longer L absorbs short stalls completely.

**Cost.** One 512 × 2 write or read is 0.35 to 0.89 µs, depending on the run. Two `memcpy`s of the same data take 33 to 66 ns. The difference is the relaxed atomic access per sample, which the compiler doesn't vectorize (§2.4 of the first doc: a plain copy of memory being written is a data race). At a 10.7 ms block that's under 0.01% of the budget. A `memcpy` would be correct on x86 in practice and undefined in C++; I kept the defined one.

**Reach for it** as the default for audio the host plays, whenever the latency is acceptable. **Skip it** when the peer's processing must sound with the host's own tracks without delay compensation (live monitoring through the peer): that's C.

### C. Position-addressed, lockstep: the host spins for the peer

**What.** The same rings. Between `send` and `receive`, the host spins until the peer has rendered what `receive` will read, or until a budget runs out, then reads whatever is there:

```cpp
bool await(std::size_t n) noexcept {
  const u64 need = clock_ - lat_ + n;
  if (budget_.count() == 0 || at(d_.out.w).load(std::memory_order_acquire) >= need) { return true; }
  const auto t0 = std::chrono::steady_clock::now();
  for (unsigned i = 1;; ++i) {
    cpu_relax();
    if (at(d_.out.w).load(std::memory_order_acquire) >= need) { return true; }
    if (i % 64 == 0 && std::chrono::steady_clock::now() - t0 > budget_) { ++r_.await_timeouts; return false; }
  }
}
```

**The latency it needs.** Here the input up to `t + n` is committed before the wait, so the peer can render up to `floor((t + n) / Q) * Q` within the callback:

> **L ≥ Q − 1** (127 frames, 2.6 ms) for arbitrary block sizes; **0** when every block is a multiple of Q starting on a quantum boundary.

The zero case is derived, not measured: every test used random block sizes.

**Measured.** 1 ms budget, peer spin-polling:

| Run | Silent after 0.5 s | Timeouts | Host callback max | Callbacks > 1 ms |
|---|---|---|---|---|
| healthy (five runs) | 0 to 1,406 | 0 to 7 | 0.70 to 1.01 ms | 0 to 7 |
| peer frozen 20 ms, 20× | 17,651 | 71 | 1.0 ms (4.4 ms in one run) | 71 |

The faults are A's and B's faults, plus a wait: every block the peer misses costs the host the whole budget, on the DAW's audio thread. That stalls every other track's processing for that budget. With the decoupled host that never happened, because there was nothing to wait for.

**Peer requirements.** To answer inside the host's callback, the peer has to be awake: spinning, as in the test, which costs a core, or woken by the host. A wake is a kernel call, which is what WAX's tick semaphore and done event are. The spin replaces the done wait; it doesn't replace the tick, if the peer sleeps between quanta.

**Reach for it** when the peer is part of a live signal path and its latency can't be compensated. Keep the budget well below the block period, and treat each timeout as a dropout the user will hear. **Skip it** for everything else: B gives the same alignment and the same fault handling with the peer unable to stall the host.

## 4. Commit order across two rings

The host writes MIDI and audio for the same positions into two different rings. A combined commit would need a third word and a second writer to it. Ordering gives the same guarantee for free. The host's release store of `in.w` comes after its MIDI commits in program order. The peer's acquire load of `in.w` happens before its MIDI reads. So if the peer sees the audio for position *p*, it sees every event stamped before *p*. The peer commits `midi_out` before `out`, and the host reads `out.w` before `midi_out`: the same argument in the other direction.

Measured with the host held for 200 µs between its two commits, the preemption the order has to survive:

| Host's order | Peer: events behind their audio | Host: echoes behind their audio | Late at the host |
|---|---|---|---|
| MIDI, stall, audio | 0 | 0 | 0 of 762 |
| audio, stall, MIDI | 588 | 587 | 588 of 762 |

Without the stall, the wrong order also gave 0 here: the window is a few nanoseconds against the peer's 50 µs poll. That is why it survives testing and fails in a session, whenever the host is preempted between the two commits.

## 5. MIDI: late, lost, stuck

- **Late** is the normal failure. The latency was too short, the peer was frozen, or a block was larger than the latency covers. The event is delivered at offset 0 of the first block that can take it. In every run without kills, 0 events were lost.
- **Lost** happens in exactly three ways, and each is counted where it happens: the lane is full (`midi_in_dropped` / `midi_out_dropped`, 0 in every run here with a 16 KiB lane, 682 events); a new peer drops events addressed to the past (`midi_in_dropped_at_start`); or the host drops a dead peer's leftovers (`midi_out_stale`). With 22 kills: 14 + 15 = 29, the whole loss.
- **Stuck** is a note-on whose note-off will never come. The only source is a peer that dies between the two. The host tracks what it has delivered (16 channels × 128 notes, a 256-byte bitset). When the peer's generation changes, it sends note-offs for everything still on and drops events from older generations. Measured: 8 notes ended that way and 0 held past due. Without it, 5 notes were held past their note-off. This test strikes every note again within about a second; a real session may never.

Size the lanes for the longest stall you want to ride through without loss. Each event takes a 24-byte frame (8 bytes of length, 16 of event), so 16 KiB holds 682. At the schedule here, about two events per 1,009 frames, that's about 7 seconds. A dense controller stream fills it hundreds of times faster.

## 6. The JUCE glue

The glue's `processBlock` copies the input MIDI out first, because `send` needs it before `receive` overwrites anything. `MidiBuffer` iteration yields events with up to 3 bytes for the wire; longer ones (SysEx) are skipped here.

```cpp
void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  if (!sized_) {                          // the host owns this buffer: it can only be grown here, once
    midi.ensureSize(kMaxOutEvents * 16);  // 4 + 2 + up to 3 bytes per event, rounded up
    sized_ = true;
  }
  const auto n = static_cast<std::size_t>(buffer.getNumSamples());
  std::size_t k = 0;
  for (const auto m : midi) {             // the host's MIDI, before anything overwrites the buffer
    if (m.numBytes < 1 || m.numBytes > 3 || k == in_.size()) continue;
    BlockEvent& e = in_[k++];
    e.offset = static_cast<utx::u32>(std::max(m.samplePosition, 0));
    e.size   = static_cast<u8>(m.numBytes);
    for (int i = 0; i < 3; ++i) e.data[i] = i < m.numBytes ? m.data[i] : u8{0};
  }
  host_.send(buffer.getArrayOfReadPointers(), numIn, n, std::span<const BlockEvent>(in_.data(), k));
  midi.clear();                           // keeps its capacity
  host_.receive(buffer.getArrayOfWritePointers(), numOut, n, [&](const Delivered& e) {
    midi.addEvent(e.data, e.size, static_cast<int>(e.offset));
  });
}
```

`prepareToPlay` calls `host_.prepare()` and `setLatencySamples(L)`. The compiled harness passes `numIn`/`numOut` as arguments; an `AudioProcessor` takes them from `getTotalNumInputChannels()` and `getTotalNumOutputChannels()`.

Allocations inside `processBlock`, counted at `malloc`/`realloc` (JUCE's `HeapBlock` calls them directly, so counting `operator new` sees none). 20,000 blocks, the peer answering each event four times, one block of twelve times that:

| Variant | Blocks 0–9 | Blocks 10–19,999 |
|---|---|---|
| build into an `ensureSize`'d member, `swapWith` the host's | 5 | 2 (blocks 179 and 2,115) |
| `clear()` and `addEvent` into the host's | 5 | 2 (blocks 179 and 818) |
| the host's `ensureSize`'d in the first block, then as above | 1 | **0** |

The swap moves the growth to other blocks but doesn't remove it. The first allocation happens either way, on the audio thread; the last variant puts it at a known time, in the first block. Size `kMaxOutEvents` to the most MIDI a block can carry, and drop (counted) beyond it, so no later block can grow the buffer. Audio in the same runs: 0 misaligned frames. MIDI: 252,788 echoes at their own sample position, 6,524 late, 0 bad.

## 7. Cross-compare

| | A. FIFO pair | B. Positions, decoupled | C. Positions, lockstep (spin) | WAX lockstep (tick + done event) |
|---|---|---|---|---|
| Latency | whatever accumulates; drifted 589 → 1,494 | fixed: M + Q − 1 + margin (735 here) | fixed: Q − 1, or 0 on aligned blocks | not measured here |
| Host waits on peer | never | never | up to the budget, per block | up to the budget, in the kernel |
| Host callback max, peer frozen | 30 µs–0.96 ms | 39 µs | budget (1 ms; 4.4 ms once) | not measured here |
| Peer stall | latency grows, permanently | silence for stall − L, then exactly L | silence and a budget-long wait per block | not measured here |
| Peer death | not tested | silence until restart; notes ended | same as B, plus waits | `Regenerate`; not measured here |
| MIDI alignment | cursor space (survives drops, shares the drift) | position, exact | position, exact | cursor space |
| Payload copy | `memcpy` | relaxed atomics, 0.35–0.89 µs per 512 × 2 | same as B | `memcpy` |
| Syscalls per block | 0 | 0 | 0 | ≥ 3 per tick (per `section-hh-upgrade.md`'s audit) |
| Peer must be awake | no | no | yes: spinning, or woken | woken by the tick |

### By direction

- **Host → peer, audio:** B's `SampleRing`, written by `send`. The host never reads the peer's progress, so a dead peer costs it nothing.
- **Host → peer, MIDI:** a `MidiLane`, committed before the audio of the same block. A full lane drops and counts; it never waits.
- **Peer → host, audio:** a `SampleRing`, read by position at `t − L`, clamped to what the host has sent.
- **Peer → host, MIDI:** a `MidiLane` tagged with the peer's generation, read after the audio. Old generations are dropped, and their open notes ended.

### Decision guide

1. Can the peer's latency be compensated (it's a track or an effect the host delays everything else for)? Use B, and report `L = M + Q − 1 + margin`. Stop there.
2. Must the peer sound in time with live input? Use C with a budget well under the block period, and accept that a stalled peer stalls the DAW for that budget per block. If the host's blocks are multiples of the peer's quantum, L can be 0.
3. Is the output only displayed, never played? A FIFO pair is fine, and its drift doesn't matter.
4. Whatever you pick: commit MIDI before audio, read audio before MIDI, stamp everything with positions, and end a dead peer's notes in the host.
5. Grow the host's `MidiBuffer` once in the first block and cap the events per block. Nothing else keeps `processBlock` allocation-free.

## 8. What it means for WAX's ring

From the excerpts in `section-hh-upgrade.md`, not from the source. `Inlet::Write` appends what fits and drops the tail once the worklet is a ring behind. `WriteMidi` stamps events from the input cursor the block began at, and `Outlet` reads from `out_read`. That is A's shape. In cursor space MIDI stays aligned with audio even across a dropped tail, which is a real virtue. What A measured here is the latency, which can grow after any stall in free-running mode. `Skip`, `reader_base_` and `Regenerate` exist and may resynchronise it. I haven't checked whether they cover a stall the page recovers from on its own.

The move to B keeps the wire's `uint32_t` words: positions mod 2³² wrap after 24.8 hours at 48 kHz. Every comparison becomes a signed 32-bit difference (§8a), and the worklet's `in_read` and `out_read` cursors stop being state the plugin reads. Lockstep maps onto C with the done-event wait replaced by a spin on `out_write`. The tick stays, because it is what wakes the patched clock.

## 8a. From TypeScript and C#

The peer in the product is an `AudioWorkletProcessor`: `process()` is called once per 128-frame quantum, so it is `PeerEnd::step` with `Q = 128` imposed rather than chosen. Three facts about JavaScript, checked in Node 22:

- `Atomics` refuses a `Float32Array` (`TypeError: ... is not an integer typed array`). Atomic access to the ring words needs `Int32Array`/`Uint32Array` views.
- `Atomics.load` on a `BigUint64Array` returns a `bigint`. 64-bit positions on the audio thread mean `BigInt` arithmetic per quantum. That's a reason to keep positions 32-bit on the wire, as `wire.hh` already does.
- 32-bit positions need wrap-safe comparison. Checked over 7,716 cases on either side of 0, 2³¹ and 2³²: 0 wrong.

```ts
export const ahead = (a: number, b: number): number => (a - b) | 0; // a - b as a signed 32-bit distance
export const add = (a: number, n: number): number => (a + n) >>> 0;

export function inputReady(w: number, p: number, q: number, ringFrames: number, maxBlock: number): "ready" | "notYet" | "lapped" {
  if (ahead(w, add(p, q)) < 0) return "notYet";
  if (ahead(p, add(w, maxBlock - ringFrames)) < 0) return "lapped";
  return "ready";
}
```

Two things differ from C++ and weren't tested here. A race on a plain `Float32Array` element isn't undefined behaviour in JavaScript (the ECMAScript memory model gives it some written value), so the payload copy could be `Float32Array.prototype.set`, which C++ can't do without a race. But JavaScript has no standalone fence: every `Atomics` operation is sequentially consistent, and whether that orders the plain payload accesses the way `SampleRing::read`'s fences do is not something I established.

From C#: the same layout through `MemoryMappedViewAccessor.SafeMemoryMappedViewHandle`, `Volatile.Read`/`Volatile.Write` on `ref ulong` for `w`, `base` and `gen`, `BitConverter.Int32BitsToSingle` per sample, and `Interlocked` nowhere: every word has one writer. Not compiled.

## 9. Verification status

**Compiled.** `duplex.hpp` and its tests: `g++ 13.3 -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Wpedantic` and `clang++ 18.1.3 -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion`, zero diagnostics. The glue against JUCE 9.0.6 (`654bdbc`, 2026-11-19): `juce_core` and `juce_audio_basics` built from source with `JUCE_USE_CURL=0`, linked with `-lz`. The TypeScript: `tsc 7.0.2 --strict --noUncheckedIndexedAccess`, run on Node 22.

**Run, x86-64 Linux, 2 vCPUs, no realtime scheduling.** The host and the peer were separate forked processes, each mapping the `shm_open` object itself. The host was paced by absolute-deadline sleeps at 48 kHz, with blocks uniformly 1–512 frames, about 1,500 blocks per 8-second run. Audio was two channels encoding the position (`t mod 2²² + 1`, the second channel + 0.5). MIDI was a note-on every 1,009 frames and its note-off 300 later, known to both sides. Each run ended with a 200 ms drain without new MIDI, so the lost counts are exact. The parent injected faults between 2 s and the last second: SIGKILL followed by `waitpid` and a 0–20 ms dead time before the restart, or SIGSTOP for 20 ms. Counts are from the last full run; where earlier runs differed, the range is given.

- Unit: lap-edge torture, 1.5 s per variant; restart gap of 512; write/read/`memcpy` timings over 200,000 iterations, three or more runs.
- System: the 13 configurations quoted in §§3–5, plus 2 seccomp runs and a seccomp negative control.
- ASan + UBSan (GCC): the unit tests and the three fault runs, clean.
- JUCE glue: three variants × 20,000 blocks, one process, with the peer stepped between blocks.

**Read in source.** JUCE 9.0.6: `processBlock`'s contract (`juce_AudioProcessor.h` in `juce_audio_processors_headless`); `MidiBuffer::addEvent`, `clear`, `ensureSize`, `swapWith` (`juce_MidiBuffer.cpp:121–162`); `ArrayBase::insert`, `createInsertSpace`, `ensureAllocatedSize`, `clear`, `swapWith` (`juce_ArrayBase.h`).

**Not verified.**

- Windows, MSVC, ARM64. The fence argument follows the C++ model; x86's ordering would hide a missing fence, and ThreadSanitizer can't model fences.
- A realtime-scheduled peer; R on a real machine; any DAW.
- Lockstep at L = 0 with aligned blocks (derived only).
- WAX's own ring, clock and resync paths: §8 reads excerpts, not the source.
- The JavaScript memory-model points in §8a, and the C# mapping.
- More than one plugin instance per mapping (one `Duplex` per instance is the design; not exercised).
