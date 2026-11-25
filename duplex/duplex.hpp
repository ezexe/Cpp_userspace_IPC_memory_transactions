// duplex.hpp: one audio callback that is producer and consumer of both audio and MIDI, against a peer
// process, without waiting on it. Self-contained apart from utx.hpp (TxRing, at(), Shareable).
//
// The shared index is the host's sample clock: every sample and every MIDI event carries the absolute
// position it belongs to. Readers ask for positions, not "the next frames", so a peer that stalls, dies or
// restarts never shifts the alignment, and nobody has a cursor the other must wait on.
#pragma once

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "../utx/utx.hpp"

namespace utx::duplex {

using u8 = std::uint8_t;

// One MIDI event on the wire. pos is absolute; gen is the writing peer's generation (0 from the host).
struct MidiEvent {
  u64 pos;
  u32 gen;
  u8 size;  // 1..3
  u8 data[3];
};
static_assert(sizeof(MidiEvent) == 16 && Shareable<MidiEvent>);

// [lo, hi): the positions a read found valid. Empty when lo >= hi.
struct Span {
  u64 lo, hi;
};

// ---------------------------------------------------------------------------------------------
// SampleRing: one writer, any readers, addressed by position. The writer appends at w; each slot is
// rewritten once per lap. A reader validates after copying, as a seqlock does, so the writer never
// looks at a reader and a lapped reader finds out instead of reading torn audio.
// ---------------------------------------------------------------------------------------------
template <std::size_t Ch, std::size_t S, std::size_t M>
  requires(std::has_single_bit(S) && M > 0 && 2 * M <= S)
struct SampleRing {
  alignas(64) u64 w;  // positions below w are committed
  u64 base;           // positions below base were never written by the current run
  u64 gen;            // the writer process's generation; bumped by each new writer
  alignas(64) u32 bits[Ch][S];

  static constexpr u64 kMask = S - 1;

  // Writer. Appends n <= M frames at w; src[c] beyond |channels| (or null) is silence.
  void write(const float* const* src, std::size_t channels, std::size_t n) noexcept {
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

  // Writer. Moves the run forward to |pos|; the skipped positions hold an older lap and never become valid.
  void rebase(u64 pos) noexcept {
    pos = std::max(pos, at(w).load(std::memory_order_relaxed));
    at(base).store(pos, std::memory_order_relaxed);
    at(w).store(pos, std::memory_order_release);
  }

  // Any thread, any process. Copies positions [t, t+n) (n <= M) into dst[c], silence wherever they aren't
  // valid, and returns the valid span. |limit| caps what the writer may claim: a reader that knows the
  // writer can't be ahead of some position passes it, and a corrupt w is clamped.
  Span read(u64 t, float* const* dst, std::size_t channels, std::size_t n, u64 limit = ~u64{0}) const noexcept {
    const u64 w1 = std::min(at(w).load(std::memory_order_acquire), limit);
    const u64 b  = at(base).load(std::memory_order_relaxed);
    u64 lo       = std::max(t, b);
    u64 hi       = std::min(t + n, w1);
    if (lo < hi) {
      for (std::size_t c = 0; c < channels && c < Ch; ++c) {
        for (u64 q = lo; q < hi; ++q) {
          dst[c][q - t] = std::bit_cast<float>(
              std::atomic_ref<const u32>(bits[c][q & kMask]).load(std::memory_order_relaxed));
        }
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      // The writer may be rewriting the slots of [w2 + M - S, w2 + M - S + M) right now, and has rewritten
      // everything below that already. What we copied from there is suspect.
      const u64 w2     = at(w).load(std::memory_order_relaxed);
      const u64 floor2 = w2 + M > S ? w2 + M - S : 0;
      lo               = std::clamp(floor2, lo, hi);
    } else {
      lo = hi = t;
    }
    for (std::size_t c = 0; c < channels; ++c) {  // silence outside [lo, hi)
      if (c >= Ch) {
        std::fill_n(dst[c], n, 0.0f);
        continue;
      }
      std::fill_n(dst[c], lo - t, 0.0f);
      std::fill_n(dst[c] + (hi - t), t + n - hi, 0.0f);
    }
    return {lo, hi};
  }
};

// ---------------------------------------------------------------------------------------------
// MidiLane: a TxRing of MidiEvents, one event per frame. A stream, not state: events are taken in order
// and acknowledged one by one, so a late event is still delivered and only a full lane loses one.
// ---------------------------------------------------------------------------------------------
template <std::size_t Cap>
struct MidiLane {
  TxRing<Cap> q;

  // Producer. False when the lane is full: the event is dropped, never waited for.
  bool push(const MidiEvent& e) noexcept {
    typename TxRing<Cap>::Tx tx(q);
    tx.append(e);
    return tx.commit();
  }

  // Consumer. Hands f each event with pos < end, at most |max| of them, in order, acknowledging each after
  // f returns. A frame that isn't an event, or a corrupt lane, empties the lane (the peer is untrusted).
  template <class F>
  std::size_t take_before(u64 end, std::size_t max, F&& f) {
    std::byte scratch[sizeof(MidiEvent)];
    std::size_t k = 0;
    while (k < max) {
      using Take = typename TxRing<Cap>::Take;
      const Take t = q.consume(scratch, [&](std::span<const std::byte> frame) {
        if (frame.size() != sizeof(MidiEvent)) { return true; }
        MidiEvent e;
        std::memcpy(&e, frame.data(), sizeof e);
        if (e.pos >= end) { return false; }
        f(e);
        ++k;
        return true;
      });
      if (t == Take::kCorrupt || t == Take::kTooBig) {
        at(q.r).store(at(q.w).load(std::memory_order_acquire), std::memory_order_release);
        return k;
      }
      if (t != Take::kDone) { return k; }
    }
    return k;
  }
};

// The mapping shared by one plugin instance and its peer. Four single-writer channels: the host writes
// in and midi_in, the peer writes out and midi_out. Nothing in it has two writers.
template <std::size_t Ch, std::size_t S, std::size_t M, std::size_t MidiCap>
struct Duplex {
  static constexpr std::size_t kChannels = Ch, kMaxBlock = M;
  SampleRing<Ch, S, M> in;
  SampleRing<Ch, S, M> out;
  MidiLane<MidiCap> midi_in;
  MidiLane<MidiCap> midi_out;
};

// A block's MIDI event as the host hands it over: an offset into the block.
struct BlockEvent {
  u32 offset;
  u8 size;
  u8 data[3];
};

// A MIDI event as a reader delivers it into a block.
struct Delivered {
  enum Kind : u8 { kOnTime, kLate, kClosing };
  u64 pos;     // the position it was stamped with (a closing note-off: the block's first)
  u32 offset;  // into the block: pos - first, or 0 for late and closing events
  Kind kind;   // kClosing: a note-off ending a note a dead peer started
  u8 size;
  u8 data[3];
};

// ---------------------------------------------------------------------------------------------
// HostEnd: what processBlock runs. Audio-thread state lives here, in private memory.
// Per block, in this order: send, (await, lockstep only), receive.
// ---------------------------------------------------------------------------------------------
template <class D>
class HostEnd {
 public:
  struct Readings {
    u64 blocks, frames_valid, frames_silent;
    u64 midi_in_dropped, midi_out_late, midi_out_stale, midi_out_bad;
    u64 gen_changes, notes_closed, await_timeouts;
  };

  // latency: how far behind the block the output is read, in frames (reported to the host as PDC).
  // budget: lockstep only; how long await() may spin for the peer. Zero: never wait.
  HostEnd(D& d, u64 latency, std::chrono::nanoseconds budget = {}) noexcept : d_(d), lat_(latency), budget_(budget) {}

  // Message thread, before the first block. The clock lives in the mapping, so positions stay monotonic
  // across re-prepares and a running peer stays aligned.
  void prepare() noexcept {
    clock_ = std::max(at(d_.in.w).load(std::memory_order_relaxed), lat_);
    d_.in.rebase(clock_);
  }

  u64 clock() const noexcept { return clock_; }
  const Readings& readings() const noexcept { return r_; }

  // Audio thread. io holds the host's input; it is copied out here, before receive() overwrites it.
  // MIDI is committed before the audio: a reader that sees the audio for a position sees its events.
  // Any n: the host may pass more than prepareToPlay's size, or zero; the ring takes it in writes of <= M.
  template <class Events>
  void send(const float* const* io, std::size_t channels, std::size_t n, const Events& events) noexcept {
    const u64 at_pos = at(d_.in.w).load(std::memory_order_relaxed);  // ours: the host is in's only writer
    for (const BlockEvent& e : events) {
      MidiEvent m{at_pos + e.offset, 0, e.size, {e.data[0], e.data[1], e.data[2]}};
      if (!d_.midi_in.push(m)) { ++r_.midi_in_dropped; }
    }
    channels = std::min(channels, D::kChannels);
    const float* part[D::kChannels];
    for (std::size_t off = 0; off < n; off += D::kMaxBlock) {
      for (std::size_t c = 0; c < channels; ++c) { part[c] = io[c] + off; }
      d_.in.write(part, channels, std::min(n - off, D::kMaxBlock));
    }
  }

  // Audio thread, lockstep only: spins until the peer has rendered what receive() will read, or the budget
  // runs out. No kernel call; the clock is a vDSO read.
  bool await(std::size_t n) noexcept {
    const u64 need = clock_ - lat_ + n;
    if (budget_.count() == 0 || at(d_.out.w).load(std::memory_order_acquire) >= need) { return true; }
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned i = 1;; ++i) {
      cpu_relax();
      if (at(d_.out.w).load(std::memory_order_acquire) >= need) { return true; }
      if (i % 64 == 0 && std::chrono::steady_clock::now() - t0 > budget_) {
        ++r_.await_timeouts;
        return false;
      }
    }
  }

  // Audio thread. Overwrites io with the peer's output for [clock - latency, +n), silence where it has none,
  // and hands emit(const Delivered&) the block's MIDI in order, late events at offset 0. Audio is read
  // before MIDI, the mirror of the peer's commit order. Advances the clock; returns the valid span.
  template <class Emit>
  Span receive(float* const* io, std::size_t channels, std::size_t n, Emit&& emit) noexcept {
    if (n == 0) { return {clock_, clock_}; }
    const u64 from = clock_ - lat_;
    // The peer can't have rendered past the input we've committed: clamp what it claims.
    const u64 limit = at(d_.in.w).load(std::memory_order_relaxed);
    Span s{0, 0};
    u64 got = 0;
    float* part[D::kChannels];
    const std::size_t ours = std::min(channels, D::kChannels);
    for (std::size_t off = 0; off < n; off += D::kMaxBlock) {
      const std::size_t len = std::min(n - off, D::kMaxBlock);
      for (std::size_t c = 0; c < ours; ++c) { part[c] = io[c] + off; }
      const Span k = d_.out.read(from + off, part, ours, len, limit);
      if (k.hi > k.lo) {
        if (s.hi == s.lo) { s.lo = k.lo; }
        s.hi = k.hi;
        got += k.hi - k.lo;
      }
    }
    for (std::size_t c = ours; c < channels; ++c) { std::fill_n(io[c], n, 0.0f); }  // outputs we don't carry
    r_.frames_valid += got;
    r_.frames_silent += n - got;
    // A new peer process can't finish the notes the old one started: end them here.
    const u64 g = at(d_.out.gen).load(std::memory_order_acquire);
    if (g != gen_) { regenerate(g, from, emit); }
    d_.midi_out.take_before(from + n, ~std::size_t{0}, [&](const MidiEvent& e) {
      if (e.gen < gen_) {
        ++r_.midi_out_stale;  // a dead peer's: its note-ons would never be ended
        return;
      }
      if (e.gen > gen_) { regenerate(e.gen, from, emit); }
      if (e.size < 1 || e.size > 3) {
        ++r_.midi_out_bad;
        return;
      }
      const bool late = e.pos < from;
      if (late) { ++r_.midi_out_late; }
      track(e.data[0], e.data[1], e.size > 2 ? e.data[2] : u8{0});
      emit(Delivered{e.pos, late ? 0u : static_cast<u32>(e.pos - from), late ? Delivered::kLate : Delivered::kOnTime,
                     e.size, {e.data[0], e.data[1], e.data[2]}});
    });
    clock_ += n;
    ++r_.blocks;
    return s;
  }

  // Audio thread. The notes the peer has started and not ended, as delivered by receive().
  std::size_t open_notes() const noexcept {
    std::size_t k = 0;
    for (u64 word : open_) { k += static_cast<std::size_t>(std::popcount(word)); }
    return k;
  }

 private:
  template <class Emit>
  void regenerate(u64 g, u64 from, Emit&& emit) noexcept {
    for (std::size_t i = 0; i < 16 * 128; ++i) {
      if ((open_[i / 64] >> (i % 64)) & 1) {
        emit(Delivered{from, 0, Delivered::kClosing, 3, {static_cast<u8>(0x80 | (i / 128)), static_cast<u8>(i % 128), 0}});
        ++r_.notes_closed;
      }
    }
    std::fill(std::begin(open_), std::end(open_), u64{0});
    gen_ = g;
    ++r_.gen_changes;
  }
  void track(u8 status, u8 note, u8 velocity) noexcept {
    const unsigned type = status & 0xF0u, i = (status & 0x0Fu) * 128u + (note & 0x7Fu);
    if (type == 0x90 && velocity > 0) {
      open_[i / 64] |= u64{1} << (i % 64);
    } else if (type == 0x80 || type == 0x90) {
      open_[i / 64] &= ~(u64{1} << (i % 64));
    }
  }

  D& d_;
  const u64 lat_;
  const std::chrono::nanoseconds budget_;
  u64 clock_ = 0, gen_ = 0;
  u64 open_[16 * 128 / 64]{};
  Readings r_{};
};

// ---------------------------------------------------------------------------------------------
// PeerEnd: what the peer's render loop runs, one quantum of Q frames at a time. Q need not divide the host's
// block size; positions are what line up.
// ---------------------------------------------------------------------------------------------
template <class D, std::size_t Q>
  requires(Q <= D::kMaxBlock)
class PeerEnd {
 public:
  static constexpr std::size_t kMaxEvents = 64;  // per quantum; the rest wait for the next one, late
  enum class Step { kRendered, kNotYet, kJumped };
  struct Readings {
    u64 quanta, jumps, midi_in_late, midi_in_dropped_at_start, midi_out_dropped;
  };

  explicit PeerEnd(D& d) noexcept : d_(d) {}

  // A new peer process. The previous one must be dead, not just silent: two writers on out is the
  // multi-writer problem, and only the kernel knows which it is. Takes the next generation and starts at
  // the host's clock; events for positions before it are dropped.
  void start() noexcept {
    gen_ = at(d_.out.gen).load(std::memory_order_relaxed) + 1;
    at(d_.out.gen).store(gen_, std::memory_order_release);
    jump(at(d_.in.w).load(std::memory_order_acquire));
    // The input clock was read first, so nothing at or after the new position is dropped.
    d_.midi_in.take_before(p_, ~std::size_t{0}, [&](const MidiEvent&) { ++r_.midi_in_dropped_at_start; });
  }

  u64 position() const noexcept { return p_; }
  u32 generation() const noexcept { return static_cast<u32>(gen_); }
  const Readings& readings() const noexcept { return r_; }

  // Renders [p, p+Q) if its input is committed. render(in, out, events, count, emit) gets Ch input and output
  // channel pointers and the quantum's MIDI as Delivered (late events at offset 0); emit(offset, size, data)
  // sends MIDI stamped at p + offset.
  template <class Render>
  Step step(Render&& render) {
    const u64 wi = at(d_.in.w).load(std::memory_order_acquire);
    if (wi < p_ + Q) { return Step::kNotYet; }
    float in[D::kChannels][Q], out[D::kChannels][Q];
    float* inp[D::kChannels];
    float* outp[D::kChannels];
    for (std::size_t c = 0; c < D::kChannels; ++c) { inp[c] = in[c], outp[c] = out[c]; }
    const Span s = d_.in.read(p_, inp, D::kChannels, Q);
    if (s.lo != p_ || s.hi != p_ + Q) {  // lapped: we fell more than a ring behind
      jump(wi);
      ++r_.jumps;
      return Step::kJumped;
    }
    Delivered ev[kMaxEvents];
    std::size_t count = 0;
    d_.midi_in.take_before(p_ + Q, kMaxEvents, [&](const MidiEvent& e) {
      if (e.size < 1 || e.size > 3) { return; }
      const bool late = e.pos < p_;
      if (late) { ++r_.midi_in_late; }
      ev[count++] = {e.pos, late ? 0u : static_cast<u32>(e.pos - p_), late ? Delivered::kLate : Delivered::kOnTime,
                     e.size, {e.data[0], e.data[1], e.data[2]}};
    });
    auto emit = [&](u32 offset, u8 size, const u8* data) {
      const MidiEvent m{p_ + std::min<u64>(offset, Q - 1), static_cast<u32>(gen_), size, {data[0], data[1], data[2]}};
      if (!d_.midi_out.push(m)) { ++r_.midi_out_dropped; }
    };
    render(static_cast<const float* const*>(inp), static_cast<float* const*>(outp), static_cast<const Delivered*>(ev),
           count, emit);
    d_.out.write(outp, D::kChannels, Q);  // after the MIDI: the host reads audio first, then MIDI
    p_ += Q;
    ++r_.quanta;
    return Step::kRendered;
  }

 private:
  // Restarts the run at the quantum holding |pos|. MIDI is kept: after a stall, the events for the skipped
  // positions are delivered late, so the note-offs among them still reach the instrument.
  void jump(u64 pos) noexcept {
    p_ = std::max(pos / Q * Q, at(d_.out.w).load(std::memory_order_relaxed));
    d_.out.rebase(p_);
  }

  D& d_;
  u64 p_ = 0, gen_ = 0;
  Readings r_{};
};

}  // namespace utx::duplex
