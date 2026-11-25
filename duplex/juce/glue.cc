// processBlock glue against JUCE's own AudioBuffer and MidiBuffer, with an in-process peer stepped between
// blocks. Checks the in-place output, the MIDI positions, and heap allocations inside processBlock.
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <span>

#include "../duplex.hpp"

// Counted at malloc, not operator new: JUCE's HeapBlock (under Array, so under MidiBuffer) calls std::malloc
// and std::realloc directly. glibc's own entry points do the work.
static bool g_counting = false;
static long g_news     = 0;
extern "C" void* __libc_malloc(std::size_t);
extern "C" void* __libc_realloc(void*, std::size_t);
extern "C" void* __libc_calloc(std::size_t, std::size_t);
extern "C" void* malloc(std::size_t n) {
  if (g_counting) { ++g_news; }
  return __libc_malloc(n);
}
extern "C" void* realloc(void* p, std::size_t n) {
  if (g_counting) { ++g_news; }
  return __libc_realloc(p, n);
}
extern "C" void* calloc(std::size_t k, std::size_t n) {
  if (g_counting) { ++g_news; }
  return __libc_calloc(k, n);
}

using namespace utx::duplex;
constexpr std::size_t kCh = 2, kM = 512, kQ = 128;
using D               = Duplex<kCh, 8192, kM, 16384>;
constexpr utx::u64 kL = kM + kQ - 1;

// --- the glue: what an AudioProcessor's prepareToPlay and processBlock would hold ---------------------
class Glue {
 public:
  static constexpr int kMaxOutEvents = 512;
  explicit Glue(D& d) : host_(d, kL) {}

  void prepareToPlay(double /*rate*/, int /*maxBlock*/) {
    out_.ensureSize(kMaxOutEvents * 16);  // 4 + 2 + up to 3 bytes per event, rounded up
    host_.prepare();                       // and setLatencySamples(kL) in the AudioProcessor
  }

  // swap: build into a preallocated member, swapWith the host's. sized: grow the host's buffer to the cap on
  // the first call, then write into it directly.
  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, int numIn, int numOut, bool swap, bool sized) {
    const auto n = static_cast<std::size_t>(buffer.getNumSamples());
    if (sized && !sized_) {  // the host owns this buffer: it can only be grown here, once, on the first block
      midi.ensureSize(kMaxOutEvents * 16);
      sized_ = true;
    }
    std::size_t k = 0;
    for (const auto m : midi) {  // the host's MIDI, before anything overwrites the buffer
      if (m.numBytes < 1 || m.numBytes > 3 || k == in_.size()) { continue; }
      BlockEvent& e = in_[k++];
      e.offset      = static_cast<utx::u32>(std::max(m.samplePosition, 0));
      e.size        = static_cast<u8>(m.numBytes);
      for (int i = 0; i < 3; ++i) { e.data[i] = i < m.numBytes ? m.data[i] : u8{0}; }
    }
    host_.send(buffer.getArrayOfReadPointers(), static_cast<std::size_t>(numIn), n, std::span<const BlockEvent>(in_.data(), k));
    juce::MidiBuffer& sink = swap ? out_ : midi;
    if (!swap) { midi.clear(); }
    host_.receive(buffer.getArrayOfWritePointers(), static_cast<std::size_t>(numOut), n, [&](const Delivered& e) {
      sink.addEvent(e.data, e.size, static_cast<int>(e.offset));
    });
    if (swap) {
      midi.swapWith(out_);  // no allocation: the two buffers trade storage
      out_.clear();         // keeps its capacity
    }
  }
  utx::u64 clock() const { return host_.clock(); }

 private:
  HostEnd<D> host_;
  juce::MidiBuffer out_;
  bool sized_ = false;
  std::array<BlockEvent, 256> in_{};
};

static float enc(utx::u64 t) { return static_cast<float>(t % (utx::u64{1} << 22) + 1); }

static void run(bool swap, bool sized = false) {
  auto* d = new D();
  Glue glue(*d);
  PeerEnd<D, kQ> peer(*d);
  peer.start();
  glue.prepareToPlay(48000.0, static_cast<int>(kM));
  juce::AudioBuffer<float> buffer(2, 4 * static_cast<int>(kM));
  juce::MidiBuffer midi;  // the host's: one object, reused every block, as a plug-in wrapper does
  std::mt19937 rng(5);
  long first_blocks = 0, later = 0, burst = 0;
  std::vector<unsigned char> sent(1 << 24, 0);  // positions the host stamped a note-on at
  utx::u64 wrong = 0, silent = 0, ok = 0, midi_ok = 0, midi_bad = 0, midi_late = 0, zero_blocks = 0, big_blocks = 0;
  for (int b = 0; b < 20000; ++b) {
    // Mostly 1..512; now and then 0 (some hosts) or up to 4x the prepared size (allowed by the contract).
    const unsigned roll = rng() % 100;
    const int n = roll == 0 ? 0 : roll == 1 ? static_cast<int>(kM + 1 + rng() % (3 * kM)) : 1 + static_cast<int>(rng() % kM);
    zero_blocks += n == 0, big_blocks += n > static_cast<int>(kM);
    buffer.setSize(2, n, false, false, true);
    const utx::u64 t = glue.clock();
    const int fan    = b == 10000 ? 48 : 4;  // one block with a burst twelve times the usual
    for (int i = 0; i < n; ++i) {
      buffer.setSample(0, i, enc(t + static_cast<utx::u64>(i)));
      buffer.setSample(1, i, -enc(t + static_cast<utx::u64>(i)));
    }
    midi.clear();
    for (int i = 0; i < n; i += 97) {
      midi.addEvent(juce::MidiMessage::noteOn(1, 60 + i % 12, static_cast<juce::uint8>(100)), i);
      sent[(t + static_cast<utx::u64>(i)) % sent.size()] = 1;
    }

    g_counting = true;
    const long before = g_news;
    glue.processBlock(buffer, midi, 2, 2, swap, sized);
    const long grew = g_news - before;
    (b < 10 ? first_blocks : b >= 10000 && b < 10010 ? burst : later) += grew;  // the burst lands kL frames on
    if (grew && b >= 10) { std::printf("  block %d (n=%d, %d MIDI events out): %ld allocation(s)\n", b, n, midi.getNumEvents(), grew); }
    g_counting = false;

    for (int i = 0; i < n; ++i) {  // the peer negates: output is the input from kL frames earlier, or silence
      const float v0 = buffer.getSample(0, i), v1 = buffer.getSample(1, i);
      const utx::u64 src = t + static_cast<utx::u64>(i) - kL;
      if (v0 == 0.0f && v1 == 0.0f) { ++silent; }
      else if (v0 == -enc(src) && v1 == enc(src)) { ++ok; }
      else { ++wrong; }
    }
    for (const auto m : midi) {  // each echo sits at the offset of the event it answers, kL frames on
      const utx::u64 pos = t - kL + static_cast<utx::u64>(m.samplePosition);
      if (m.numBytes == 3 && (m.data[0] & 0xF0) == 0x90 && sent[pos % sent.size()]) { ++midi_ok; }
      else if (m.samplePosition == 0 || m.data[2] == 1) { ++midi_late; }  // late at the host, or at the peer
      else {
        ++midi_bad;
        if (midi_bad <= 4) {
          std::printf("  bad: block %d n=%d t=%llu sp=%d pos=%llu bytes=%d status=%02x\n", b, n, (unsigned long long)t, m.samplePosition,
                      (unsigned long long)pos, m.numBytes, m.data[0]);
        }
      }
    }
    while (peer.step([fan](const float* const* in, float* const* out, const Delivered* ev, std::size_t count, auto&& emit) {
             for (std::size_t c = 0; c < kCh; ++c) {
               for (std::size_t i = 0; i < kQ; ++i) { out[c][i] = -in[c][i]; }
             }
             // Answers each event `fan` times: more MIDI out than came in, as an arpeggiator or a closing burst does.
             for (std::size_t i = 0; i < count; ++i) {
               const u8 echo[3] = {ev[i].data[0], ev[i].data[1], ev[i].kind == Delivered::kLate ? u8{1} : ev[i].data[2]};
               for (int f = 0; f < fan; ++f) { emit(ev[i].offset, ev[i].size, echo); }
             }
           }) != PeerEnd<D, kQ>::Step::kNotYet) {}
  }
  std::printf("%s: allocations in processBlock, blocks 0-9: %ld, burst: %ld, blocks after: %ld | frames ok=%llu silent=%llu wrong=%llu | midi echoes at their own position=%llu late=%llu bad=%llu | zero-size blocks=%llu, oversize=%llu\n",
              swap ? "ensureSize'd member + swapWith" : sized ? "host's ensureSize'd on block 0" : "clear + addEvent into the host's", first_blocks, burst, later,
              (unsigned long long)ok, (unsigned long long)silent, (unsigned long long)wrong, (unsigned long long)midi_ok,
              (unsigned long long)midi_late, (unsigned long long)midi_bad, (unsigned long long)zero_blocks, (unsigned long long)big_blocks);
  delete d;
}

int main() {
  {  // the counter sees MidiBuffer's growth: a fresh buffer, 100 events
    juce::MidiBuffer probe;
    g_counting = true;
    const long before = g_news;
    for (int i = 0; i < 100; ++i) { probe.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), i); }
    const long grew = g_news - before;
    probe.clear();
    const long before2 = g_news;
    for (int i = 0; i < 100; ++i) { probe.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), i); }
    g_counting = false;
    std::printf("probe: 100 addEvents into a fresh MidiBuffer: %ld allocations; again after clear(): %ld\n", grew, g_news - before2);
  }
  run(true);
  run(false);
  run(false, true);
}
