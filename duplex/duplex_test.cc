// Cross-process tests for duplex.hpp. A host process plays a DAW: processBlock-shaped calls of random size
// (1..512 frames) on a 48 kHz schedule, in place on one buffer, MIDI in and out. A peer process renders
// 128-frame quanta: audio negated, each MIDI event echoed an octave up. The parent kills or freezes the peer.
//
// The input is the position itself (frame t carries t mod 2^22 + 1), so every output frame says which input
// frame it came from: a wrong value is a misalignment or a tear, never noise.
#include "duplex.hpp"

#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string>
#include <vector>

using namespace utx;
using namespace utx::duplex;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kCh = 2, kS = 8192, kM = 512, kQ = 128, kMidiCap = 16384;
using D = Duplex<kCh, kS, kM, kMidiCap>;
constexpr double kRate = 48000.0;

// ---- the signal and the MIDI schedule, known to both sides ------------------------------------
static float enc(u64 t) { return static_cast<float>(t % (u64{1} << 22) + 1); }
constexpr u64 kPeriod = 1009, kPhase = 17, kHold = 300;  // note-on at k*1009+17, its note-off 300 later
static u8 note_of(u64 k) { return static_cast<u8>(36 + k % 48); }
static u64 next_event(u64 x) {
  auto from = [x](u64 first) { return x <= first ? first : first + (x - first + kPeriod - 1) / kPeriod * kPeriod; };
  return std::min(from(kPhase), from(kPhase + kHold));
}
static bool scheduled(u64 pos, u8 out[3]) {
  if (pos >= kPhase && (pos - kPhase) % kPeriod == 0) {
    out[0] = 0x90, out[1] = note_of((pos - kPhase) / kPeriod), out[2] = 100;
    return true;
  }
  if (pos >= kPhase + kHold && (pos - kPhase - kHold) % kPeriod == 0) {
    out[0] = 0x80, out[1] = note_of((pos - kPhase - kHold) / kPeriod), out[2] = 0;
    return true;
  }
  return false;
}
static u64 count_scheduled(u64 lo, u64 hi) {
  u64 k = 0;
  for (u64 p = next_event(lo); p < hi; p = next_event(p + 1)) { ++k; }
  return k;
}

// ---- the FIFO pair: the cursor-addressed shape, for comparison -----------------------------
struct Fifo {
  alignas(64) u64 w;
  alignas(64) u64 r;
  alignas(64) float buf[kCh][kS];
};

enum Mode : u32 { kDecoupled, kLockstep, kFifo };
enum Fault : u32 { kNone, kKill, kStop };

struct Results {
  u64 blocks, ok, silent, wrong, silent_after;
  u64 cb_max_ns, cb_over_100us, cb_over_1ms;
  u64 echo_on_time, echo_retimed, echo_bad, closing, expected_echoes, midi_after_audio, stuck, overhang_max;
  u64 lat_early, lat_late;  // FIFO: median latency in frames, early window and after the faults
  u64 midi_in_dropped, midi_out_late, midi_out_stale, gen_changes, await_timeouts;
};
struct PeerTotals {
  u64 quanta, jumps, late, dropped_at_start, midi_missing, out_dropped;
};
struct Control {
  u32 mode, fault, reversed, no_gen, spin_pace, seccomp;
  u64 gap_ns;  // a stall between the two commits: the host preempted mid-send
  u64 max_block;
  u32 plant;   // seccomp control: one getpid in the loop
  u64 latency, budget_ns, duration_ms;
  u64 kills, stops;
  u64 host_done;
  u64 sent_until;  // host: where it stopped sending MIDI (the drain); ~0 before
  Results res;
  PeerTotals peer;
};
struct Shm {
  D d;
  Fifo fin, fout;
  Control c;
};

static std::string g_name;
static Shm* map_shm(bool create) {
  const int fd = shm_open(g_name.c_str(), create ? (O_CREAT | O_TRUNC | O_RDWR) : O_RDWR, 0600);
  if (fd < 0) { perror("shm_open"), exit(2); }
  if (create && ftruncate(fd, sizeof(Shm)) != 0) { perror("ftruncate"), exit(2); }
  void* p = mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) { perror("mmap"), exit(2); }
  return create ? new (p) Shm() : static_cast<Shm*>(p);
}
static void sleep_ns(long ns) {
  timespec ts{ns / 1000000000, ns % 1000000000};
  nanosleep(&ts, nullptr);
}
static void add(u64& w, u64 v) { at(w).fetch_add(v, std::memory_order_relaxed); }

// Only exit and exit_group: anything else kills the process (SIGSYS).
static void no_syscalls() {
  sock_filter f[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit, 2, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit_group, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog prog{sizeof f / sizeof f[0], f};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) { _exit(3); }
}

// ---- the peer ------------------------------------------------------------------------------
[[noreturn]] static void run_peer() {
  Shm* m           = map_shm(false);
  const Control& c = m->c;
  if (c.mode == kFifo) {
    float in[kCh][kQ];
    while (!at(c.host_done).load(std::memory_order_relaxed)) {
      const u64 ir = at(m->fin.r).load(std::memory_order_relaxed), iw = at(m->fin.w).load(std::memory_order_acquire);
      const u64 ow = at(m->fout.w).load(std::memory_order_relaxed), orr = at(m->fout.r).load(std::memory_order_acquire);
      if (iw - ir < kQ || kS - (ow - orr) < kQ) {
        sleep_ns(50'000);
        continue;
      }
      for (std::size_t ch = 0; ch < kCh; ++ch) {
        for (std::size_t i = 0; i < kQ; ++i) { in[ch][i] = m->fin.buf[ch][(ir + i) & (kS - 1)]; }
      }
      at(m->fin.r).store(ir + kQ, std::memory_order_release);
      for (std::size_t ch = 0; ch < kCh; ++ch) {
        for (std::size_t i = 0; i < kQ; ++i) { m->fout.buf[ch][(ow + i) & (kS - 1)] = -in[ch][i]; }
      }
      at(m->fout.w).store(ow + kQ, std::memory_order_release);
    }
    _exit(0);
  }
  PeerEnd<D, kQ> peer(m->d);
  peer.start();
  PeerEnd<D, kQ>::Readings prev{};
  auto render = [&](const float* const* in, float* const* out, const Delivered* ev, std::size_t count, auto&& emit) {
    for (std::size_t ch = 0; ch < kCh; ++ch) {
      for (std::size_t i = 0; i < kQ; ++i) { out[ch][i] = -in[ch][i]; }
    }
    // Did every event for this quantum's positions arrive with its audio?
    const u64 p = peer.position();
    u64 on_time = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const Delivered& e = ev[i];
      if (e.kind == Delivered::kOnTime) { ++on_time; }
      // Late events are echoed now, not at their position; velocity 1 marks them for the host's check.
      const u8 echo[3] = {e.data[0], static_cast<u8>(e.data[1] + 12), e.kind == Delivered::kLate ? u8{1} : e.data[2]};
      emit(e.offset, e.size, echo);
    }
    const u64 want = count_scheduled(p, p + kQ);
    if (on_time < want && p + kQ <= at(m->c.sent_until).load(std::memory_order_acquire)) {
      add(m->c.peer.midi_missing, want - on_time);
    }
  };
  for (;;) {
    const auto step = peer.step(render);
    const auto& r   = peer.readings();
    add(m->c.peer.quanta, r.quanta - prev.quanta);
    add(m->c.peer.jumps, r.jumps - prev.jumps);
    add(m->c.peer.late, r.midi_in_late - prev.midi_in_late);
    add(m->c.peer.dropped_at_start, r.midi_in_dropped_at_start - prev.midi_in_dropped_at_start);
    add(m->c.peer.out_dropped, r.midi_out_dropped - prev.midi_out_dropped);
    prev = r;
    if (step == PeerEnd<D, kQ>::Step::kNotYet) {
      if (at(c.host_done).load(std::memory_order_relaxed)) { _exit(0); }
      if (c.mode == kLockstep) {
        for (int i = 0; i < 64; ++i) { cpu_relax(); }
      } else {
        sleep_ns(50'000);
      }
    }
  }
}

// ---- the host ------------------------------------------------------------------------------
[[noreturn]] static void run_host() {
  Shm* m     = map_shm(false);
  Control& c = m->c;
  Results res{};
  const u64 L = c.latency;
  HostEnd<D> host(m->d, L, std::chrono::nanoseconds(c.budget_ns));
  host.prepare();
  u64 t = c.mode == kFifo ? 0 : host.clock();
  const u64 first = t;
  // no_gen: the same receive with the generation logic taken out, built from the primitives.
  u64 ng_from = 0;

  float buf[kCh][4 * kM];
  float* io[kCh] = {buf[0], buf[1]};
  std::vector<BlockEvent> events;
  std::vector<Delivered> out;
  events.reserve(64);
  out.reserve(4096);
  std::vector<u64> lat_early, lat_late;
  lat_early.reserve(1 << 20);
  lat_late.reserve(1 << 20);
  u64 open_pos[128];  // open notes in what we delivered, by note: the position they opened at, or 0
  bool flagged[128]{};
  std::fill(std::begin(open_pos), std::end(open_pos), u64{0});

  std::mt19937 rng(7);
  if (c.seccomp) { no_syscalls(); }
  const auto t0 = Clock::now();
  auto next     = t0;
  const auto end = t0 + std::chrono::milliseconds(c.duration_ms);
  u64 last_from = 0, sent_until = ~u64{0};  // after the run: 200 ms more blocks, no new MIDI, so echoes land
  for (;;) {
    const auto now = Clock::now();
    if (now >= end + std::chrono::milliseconds(200)) { break; }
    const bool draining = now >= end;
    if (draining && sent_until == ~u64{0}) {
      sent_until = t;
      at(c.sent_until).store(t, std::memory_order_release);
    }
    const std::size_t n = 1 + rng() % c.max_block;
    // The block's input and MIDI, as a host hands them over.
    for (std::size_t i = 0; i < n; ++i) { buf[0][i] = enc(t + i), buf[1][i] = enc(t + i) + 0.5f; }
    events.clear();
    for (u64 p = next_event(t); !draining && p < t + n; p = next_event(p + 1)) {
      BlockEvent e{static_cast<u32>(p - t), 3, {}};
      scheduled(p, e.data);
      events.push_back(e);
    }
    out.clear();
    const auto cb0 = Clock::now();
    Span s{0, 0};
    u64 from = t >= L ? t - L : 0;
    if (c.mode == kFifo) {
      // FIFO: append what fits, read what's there. Positions are whatever the cursors say.
      const u64 w = at(m->fin.w).load(std::memory_order_relaxed), r = at(m->fin.r).load(std::memory_order_acquire);
      const u64 take = std::min<u64>(n, kS - (w - r));
      for (std::size_t ch = 0; ch < kCh; ++ch) {
        for (u64 i = 0; i < take; ++i) { m->fin.buf[ch][(w + i) & (kS - 1)] = buf[ch][i]; }
      }
      at(m->fin.w).store(w + take, std::memory_order_release);
      const u64 orr = at(m->fout.r).load(std::memory_order_relaxed), ow = at(m->fout.w).load(std::memory_order_acquire);
      const u64 got = std::min<u64>(n, ow - orr);
      for (std::size_t ch = 0; ch < kCh; ++ch) {
        for (u64 i = 0; i < n; ++i) { buf[ch][i] = i < got ? m->fout.buf[ch][(orr + i) & (kS - 1)] : 0.0f; }
      }
      at(m->fout.r).store(orr + got, std::memory_order_release);
    } else if (c.no_gen) {
      for (const BlockEvent& e : events) {
        m->d.midi_in.push(MidiEvent{t + e.offset, 0, e.size, {e.data[0], e.data[1], e.data[2]}});
      }
      m->d.in.write(io, kCh, n);
      s = m->d.out.read(from, io, kCh, n, t + n);
      m->d.midi_out.take_before(from + n, ~std::size_t{0}, [&](const MidiEvent& e) {
        const bool late = e.pos < from;
        out.push_back(Delivered{e.pos, late ? 0u : static_cast<u32>(e.pos - from), late ? Delivered::kLate : Delivered::kOnTime,
                                e.size, {e.data[0], e.data[1], e.data[2]}});
      });
      ng_from = from;
    } else {
      if (c.gap_ns) {  // send's two commits by hand, in either order, with the host stalled between them
        auto midi = [&] {
          for (const BlockEvent& e : events) {
            m->d.midi_in.push(MidiEvent{t + e.offset, 0, e.size, {e.data[0], e.data[1], e.data[2]}});
          }
        };
        auto gap = [&] {
          const auto g0 = Clock::now();
          while (Clock::now() - g0 < std::chrono::nanoseconds(c.gap_ns)) { cpu_relax(); }
        };
        if (c.reversed) {
          m->d.in.write(io, kCh, n), gap(), midi();
        } else {
          midi(), gap(), m->d.in.write(io, kCh, n);
        }
      } else {
        host.send(io, kCh, n, events);
      }
      if (c.mode == kLockstep) { host.await(n); }
      s = host.receive(io, kCh, n, [&](const Delivered& e) { out.push_back(e); });
    }
    const auto cb_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - cb0).count());
    if (!draining) {
      res.cb_max_ns = std::max(res.cb_max_ns, cb_ns);
      res.cb_over_100us += cb_ns > 100'000;
      res.cb_over_1ms += cb_ns > 1'000'000;
      ++res.blocks;

      // Audio: each frame is the negated input of its own position, or silence.
      const double sec = std::chrono::duration<double>(now - t0).count();
      for (std::size_t i = 0; i < n; ++i) {
        const float v0 = buf[0][i], v1 = buf[1][i];
        if (v0 == 0.0f && v1 == 0.0f) {
          ++res.silent;
          res.silent_after += sec > 0.5;
        } else if (c.mode == kFifo) {
          const u64 src = static_cast<u64>(-v0) - 1, mod = u64{1} << 22;
          const u64 lat = ((t + i) % mod + mod - src) % mod;
          if (v1 != v0 - 0.5f) { ++res.wrong; }
          else { ++res.ok; }
          if (sec > 0.5 && sec < 1.5) { lat_early.push_back(lat); }
          if (sec > static_cast<double>(c.duration_ms) / 1000.0 - 1.0) { lat_late.push_back(lat); }
        } else if (v0 == -enc(from + i) && v1 == -(enc(from + i) + 0.5f)) {
          ++res.ok;
        } else {
          ++res.wrong;
        }
      }
    }
    // MIDI: every on-time echo is a scheduled event an octave up, at its own offset.
    if (c.mode != kFifo) {
      u64 on_time_in_span = 0;
      for (const Delivered& e : out) {
        if (e.kind == Delivered::kClosing) {
          ++res.closing;
        } else {
          u8 want[3];
          const bool sched = scheduled(e.pos, want);
          if (e.kind == Delivered::kOnTime && sched && e.data[0] == want[0] && e.data[1] == want[1] + 12 &&
              e.offset == e.pos - from) {
            ++res.echo_on_time;
            if (e.pos >= s.lo && e.pos < s.hi) { ++on_time_in_span; }
          } else if (!sched || e.kind == Delivered::kLate || e.data[2] == 1) {
            ++res.echo_retimed;  // the peer delivered it late and echoed it at its own position
          } else {
            ++res.echo_bad;
          }
        }
        const u8 note = static_cast<u8>(e.data[1] & 0x7F), type = static_cast<u8>(e.data[0] & 0xF0);
        if (type == 0x90 && e.data[2] > 0) {
          if (open_pos[note] != 0 && flagged[note]) {  // never ended: it sounded until struck again
            res.overhang_max = std::max(res.overhang_max, from + e.offset - (open_pos[note] - 1 + kHold));
          }
          open_pos[note] = e.pos + 1, flagged[note] = false;
        } else if (type == 0x80 || type == 0x90) {
          if (open_pos[note] != 0 && flagged[note]) {  // how long past its due note-off it sounded
            res.overhang_max = std::max(res.overhang_max, from + e.offset - (open_pos[note] - 1 + kHold));
          }
          open_pos[note] = 0;
        }
      }
      // Stuck: still sounding a full block after its note-off was due. Counted once per note-on.
      for (std::size_t k = 0; k < 128; ++k) {
        if (open_pos[k] != 0 && !flagged[k] && open_pos[k] - 1 + kHold + kM < from && open_pos[k] - 1 + kHold < sent_until) {
          ++res.stuck, flagged[k] = true;
        }
      }
      if (s.hi > s.lo && !draining) {
        const u64 want = count_scheduled(std::max(s.lo, first), s.hi);
        if (on_time_in_span < want) { res.midi_after_audio += want - on_time_in_span; }
      }
    }
    last_from = from + n;
    if (c.plant && res.blocks == 100) { (void)getpid(); }
    t += n;
    // Wait for the next block, as the device would call us.
    next += std::chrono::nanoseconds(static_cast<long long>(static_cast<double>(n) / kRate * 1e9));
    if (c.spin_pace) {
      while (Clock::now() < next) { cpu_relax(); }
    } else {
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(next.time_since_epoch()).count();
      timespec ts{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    }
  }
  (void)ng_from;
  for (std::size_t k = 0; k < 128; ++k) {
    if (open_pos[k] != 0 && flagged[k]) { res.overhang_max = std::max(res.overhang_max, last_from - (open_pos[k] - 1 + kHold)); }
  }
  if (c.mode != kFifo) {
    res.expected_echoes = count_scheduled(first, sent_until);
    const auto& r       = host.readings();
    res.midi_in_dropped = r.midi_in_dropped, res.midi_out_late = r.midi_out_late, res.midi_out_stale = r.midi_out_stale;
    res.gen_changes = r.gen_changes, res.await_timeouts = r.await_timeouts;
  }
  auto median = [](std::vector<u64>& v) -> u64 {
    if (v.empty()) { return 0; }
    std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
    return v[v.size() / 2];
  };
  res.lat_early = median(lat_early);
  res.lat_late  = median(lat_late);
  std::memcpy(&c.res, &res, sizeof res);
  at(c.host_done).store(1, std::memory_order_release);
  _exit(0);
}

template <class F>
static pid_t spawn(F&& f) {
  const pid_t pid = fork();
  if (pid == 0) { f(); }
  return pid;
}

struct Config {
  const char* label;
  Mode mode;
  u64 latency;
  Fault fault = kNone;
  u64 budget_us = 0;
  bool reversed = false, no_gen = false, seccomp = false;
  u64 duration_ms = 8000;
  u64 gap_us      = 0;
  bool plant      = false;
  u64 max_block   = kM;
};

static void run(const Config& k) {
  Shm* m           = map_shm(true);
  m->c.mode        = k.mode;
  m->c.fault       = k.fault;
  m->c.reversed    = k.reversed;
  m->c.no_gen      = k.no_gen;
  m->c.seccomp     = k.seccomp;
  m->c.spin_pace   = k.seccomp;
  m->c.latency     = k.latency;
  m->c.budget_ns   = k.budget_us * 1000;
  m->c.duration_ms = k.duration_ms;
  m->c.gap_ns      = k.gap_us * 1000;
  m->c.plant       = k.plant;
  m->c.max_block   = k.max_block;
  m->c.sent_until  = ~u64{0};
  pid_t peer       = spawn(run_peer);
  sleep_ns(20'000'000);  // let the peer start before the host's first block
  const pid_t host = spawn(run_host);
  std::mt19937 rng(11);
  const auto t0 = Clock::now();
  int status    = 0;
  for (;;) {
    if (waitpid(host, &status, WNOHANG) == host) { break; }
    const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    const bool window = sec > 2.0 && sec < static_cast<double>(k.duration_ms) / 1000.0 - 1.0;
    if (!window || k.fault == kNone) {
      sleep_ns(5'000'000);
      continue;
    }
    sleep_ns((100 + static_cast<long>(rng() % 300)) * 1'000'000);
    if (k.fault == kKill) {
      kill(peer, SIGKILL);
      waitpid(peer, nullptr, 0);  // dead and reaped: only then may a new writer start
      sleep_ns(static_cast<long>(rng() % 20) * 1'000'000);
      peer = spawn(run_peer);
      ++m->c.kills;
    } else {
      kill(peer, SIGSTOP);
      sleep_ns(20'000'000);
      kill(peer, SIGCONT);
      ++m->c.stops;
    }
  }
  kill(peer, SIGKILL);
  waitpid(peer, nullptr, 0);
  const Results& r    = m->c.res;
  const PeerTotals& p = m->c.peer;
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    printf("%-34s host died: %s %d\n", k.label, WIFSIGNALED(status) ? "signal" : "exit",
           WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
  }
  printf("%-34s L=%-4llu kills=%llu stops=%llu | blocks=%llu frames ok=%llu silent=%llu (after 0.5 s: %llu) wrong=%llu | cb max=%.1fus >100us=%llu >1ms=%llu",
         k.label, (unsigned long long)k.latency, (unsigned long long)m->c.kills, (unsigned long long)m->c.stops,
         (unsigned long long)r.blocks, (unsigned long long)r.ok, (unsigned long long)r.silent,
         (unsigned long long)r.silent_after, (unsigned long long)r.wrong,
         static_cast<double>(r.cb_max_ns) / 1000.0, (unsigned long long)r.cb_over_100us, (unsigned long long)r.cb_over_1ms);
  if (k.mode == kFifo) {
    printf(" | latency median early=%llu late=%llu frames\n", (unsigned long long)r.lat_early, (unsigned long long)r.lat_late);
  } else {
    const u64 got = r.echo_on_time + r.echo_retimed;
    printf("\n%-34s   midi: expected=%llu on_time=%llu retimed=%llu bad=%llu lost=%lld | after_audio host=%llu peer=%llu | closing=%llu stale=%llu gens=%llu held past due=%llu (max %llu frames) | peer jumps=%llu late=%llu dropped_at_start=%llu | await timeouts=%llu\n",
           "", (unsigned long long)r.expected_echoes, (unsigned long long)r.echo_on_time, (unsigned long long)r.echo_retimed,
           (unsigned long long)r.echo_bad, static_cast<long long>(r.expected_echoes) - static_cast<long long>(got),
           (unsigned long long)r.midi_after_audio, (unsigned long long)p.midi_missing, (unsigned long long)r.closing,
           (unsigned long long)r.midi_out_stale, (unsigned long long)r.gen_changes, (unsigned long long)r.stuck,
           (unsigned long long)r.overhang_max,
           (unsigned long long)p.jumps, (unsigned long long)p.late, (unsigned long long)p.dropped_at_start,
           (unsigned long long)r.await_timeouts);
  }
  munmap(m, sizeof(Shm));
}

// ---- unit tests on SampleRing alone ------------------------------------------------------------
using Ring = SampleRing<1, 1024, 128>;
struct RingShm {
  Ring ring;
  u64 stop;
};

// A writer flat out, a reader asking for the oldest positions it may still get: the edge of the lap.
static void lap_torture(bool validate) {
  void* p   = mmap(nullptr, sizeof(RingShm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  auto* s   = new (p) RingShm();
  pid_t pid = fork();
  if (pid == 0) {
    float block[128];
    for (u64 t = 0; !at(s->stop).load(std::memory_order_relaxed); t += 128) {
      for (u64 i = 0; i < 128; ++i) { block[i] = enc(t + i); }
      const float* src[1] = {block};
      s->ring.write(src, 1, 128);
    }
    _exit(0);
  }
  u64 reads = 0, frames = 0, wrong = 0;
  float dst[128];
  float* d[1] = {dst};
  const auto end = Clock::now() + std::chrono::milliseconds(1500);
  while (Clock::now() < end) {
    const u64 w = at(s->ring.w).load(std::memory_order_acquire);
    if (w < 1024) { continue; }
    const u64 t = w - 1024 + 128;  // the oldest range the rule allows right now
    u64 lo = t, hi = t + 128;
    if (validate) {
      const Span sp = s->ring.read(t, d, 1, 128);
      lo = sp.lo, hi = sp.hi;
    } else {  // the same copy without the second look at w
      for (u64 q = t; q < t + 128; ++q) {
        dst[q - t] = std::bit_cast<float>(std::atomic_ref<const u32>(s->ring.bits[0][q & 1023]).load(std::memory_order_relaxed));
      }
    }
    ++reads;
    for (u64 q = lo; q < hi; ++q) {
      ++frames;
      if (dst[q - t] != enc(q)) { ++wrong; }
    }
  }
  at(s->stop).store(1);
  waitpid(pid, nullptr, 0);
  printf("lap edge, %s: reads=%llu frames accepted=%llu wrong=%llu\n", validate ? "validated" : "no second look",
         (unsigned long long)reads, (unsigned long long)frames, (unsigned long long)wrong);
  munmap(p, sizeof(RingShm));
}

// A writer that moves its run forward: with rebase the skipped positions read as silence; with a bare
// store of w they read as the previous lap.
static void jump_gap(bool use_base) {
  auto* s = new RingShm();
  float block[128];
  const float* src[1] = {block};
  for (u64 t = 0; t < 4096; t += 128) {
    for (u64 i = 0; i < 128; ++i) { block[i] = enc(t + i); }
    s->ring.write(src, 1, 128);
  }
  const u64 to = 4096 + 512;  // the writer restarts 512 positions on
  if (use_base) {
    s->ring.rebase(to);
  } else {
    at(s->ring.w).store(to, std::memory_order_release);
  }
  for (u64 i = 0; i < 128; ++i) { block[i] = enc(to + i); }
  s->ring.write(src, 1, 128);
  float dst[128];
  float* d[1] = {dst};
  u64 stale = 0, silent = 0, ok = 0;
  for (u64 t = 4096; t < to + 128; t += 128) {
    s->ring.read(t, d, 1, 128);
    for (u64 i = 0; i < 128; ++i) {
      if (dst[i] == 0.0f) { ++silent; }
      else if (dst[i] == enc(t + i)) { ++ok; }
      else { ++stale; }
    }
  }
  printf("restart gap of 512, %s: ok=%llu silent=%llu stale-lap=%llu\n", use_base ? "rebase" : "bare store of w",
         (unsigned long long)ok, (unsigned long long)silent, (unsigned long long)stale);
  delete s;
}

static void bench() {
  auto* d = new D();
  HostEnd<D> host(*d, kM + kQ - 1 + 96);
  host.prepare();
  PeerEnd<D, kQ> peer(*d);
  peer.start();
  float buf[kCh][kM];
  float* io[kCh] = {buf[0], buf[1]};
  std::vector<BlockEvent> none;
  u64 sink        = 0;
  const int iters = 200'000;
  for (int i = 0; i < 4; ++i) {  // fill the pipe so receive copies real frames
    host.send(io, kCh, kM, none);
    while (peer.step([](const float* const*, float* const* o, const Delivered*, std::size_t, auto&&) { o[0][0] = 1; }) ==
           PeerEnd<D, kQ>::Step::kRendered) {}
    host.receive(io, kCh, kM, [&](const Delivered&) {});
  }
  auto t0 = Clock::now();
  for (int i = 0; i < iters; ++i) { d->in.write(io, kCh, kM); }
  const double host_ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / iters;
  // The copy alone: relaxed atomic loads against memcpy, 512 frames x 2 channels.
  float dst[kCh][kM];
  float* dp[kCh] = {dst[0], dst[1]};
  t0             = Clock::now();
  for (int i = 0; i < iters; ++i) { sink += d->in.read(host.clock() - kM, dp, kCh, kM).hi; }
  const double atomic_ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / iters;
  t0                     = Clock::now();
  for (int i = 0; i < iters; ++i) {
    std::memcpy(dst[0], d->in.bits[0] + (i % 8) * 512, kM * 4);
    std::memcpy(dst[1], d->in.bits[1] + (i % 8) * 512, kM * 4);
    sink += static_cast<u64>(dst[0][static_cast<std::size_t>(i) % kM]);
  }
  const double memcpy_ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / iters;
  printf("one process, 512 frames x 2 ch: SampleRing::write %.0f ns | SampleRing::read %.0f ns | 2x memcpy %.0f ns%s\n", host_ns,
         atomic_ns, memcpy_ns, sink == 42 ? " " : "");
  delete d;
}

int main(int argc, char** argv) {
  g_name = "/utx-duplex-" + std::to_string(getpid());
  setvbuf(stdout, nullptr, _IONBF, 0);
  const std::string what = argc > 1 ? argv[1] : "all";
  const auto is          = [&](const char* s) { return what == "all" || what == s; };
  const u64 Lmin = kQ - 1, Lblock = kM + kQ - 1, Lsafe = kM + kQ - 1 + 96;
  if (is("unit")) {
    lap_torture(true);
    lap_torture(false);
    jump_gap(true);
    jump_gap(false);
    bench();
  }
  if (is("latency")) {
    run({"decoupled, L = Q-1", kDecoupled, Lmin});
    run({"decoupled, L = M+Q-1", kDecoupled, Lblock});
    run({"decoupled, L = M+Q-1+2ms", kDecoupled, Lsafe});
    run({"blocks up to 4M, L for M", kDecoupled, Lsafe, kNone, 0, false, false, false, 8000, 0, false, 4 * kM});
  }
  if (is("order")) {
    run({"MIDI, 200us stall, audio", kDecoupled, Lsafe, kNone, 0, false, false, false, 8000, 200});
    run({"audio, 200us stall, MIDI", kDecoupled, Lsafe, kNone, 0, true, false, false, 8000, 200});
  }
  if (is("faults")) {
    run({"decoupled, peer killed", kDecoupled, Lsafe, kKill});
    run({"decoupled, peer killed, no gen", kDecoupled, Lsafe, kKill, 0, false, true});
    run({"decoupled, peer frozen 20 ms", kDecoupled, Lsafe, kStop});
  }
  if (is("lockstep")) {
    run({"lockstep 1 ms budget", kLockstep, Lmin, kNone, 1000});
    run({"lockstep 1 ms budget, frozen", kLockstep, Lmin, kStop, 1000});
  }
  if (is("fifo")) {
    run({"fifo pair", kFifo, 0});
    run({"fifo pair, peer frozen 20 ms", kFifo, 0, kStop});
  }
  if (is("pure")) {
    run({"decoupled, host under seccomp", kDecoupled, Lsafe, kNone, 0, false, false, true, 4000});
    run({"lockstep, host under seccomp", kLockstep, Lmin, kNone, 1000, false, false, true, 4000});
    run({"control: seccomp + one getpid", kDecoupled, Lsafe, kNone, 0, false, false, true, 4000, 0, true});
  }
  shm_unlink(g_name.c_str());
  return 0;
}
