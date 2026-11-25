// Cross-process tests for utx.hpp: fork, map the same shm object again in each child (so every
// process has it at its own address), SIGKILL writers at random instants, check invariants.
#include "utx.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string>

using namespace utx;
using Clock = std::chrono::steady_clock;

static std::string g_name;

template <class L>
static L* map_shared(bool create) {
  const int fd = shm_open(g_name.c_str(), create ? (O_CREAT | O_TRUNC | O_RDWR) : O_RDWR, 0600);
  if (fd < 0) { perror("shm_open"), exit(2); }
  if (create && ftruncate(fd, sizeof(L)) != 0) { perror("ftruncate"), exit(2); }
  void* p = mmap(nullptr, sizeof(L), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) { perror("mmap"), exit(2); }
  return create ? new (p) L() : static_cast<L*>(p);  // fresh shm pages are zero; L() keeps them zero
}

static void sleep_us(long us) {
  timespec ts{us / 1000000, (us % 1000000) * 1000};
  nanosleep(&ts, nullptr);
}

template <class F>
static pid_t spawn(F&& child) {
  const pid_t pid = fork();
  if (pid == 0) {
    child();
    _exit(0);
  }
  return pid;
}
static void kill_reap(pid_t pid) {
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);  // a zombie still answers kill(pid, 0); reap before anyone asks
}

struct Big {
  u64 a[512];  // 4 KiB: a write is long enough for a kill to land inside it
};

// ---- A / B: single-writer cells ----------------------------------------------------------------
template <template <class> class Cell>
static void cell_test(const char* label, int kills) {
  struct L {
    Cell<Big> cell;
    u64 parent_addr;
    u64 child_addr;
  };
  L* m = map_shared<L>(true);
  m->parent_addr = reinterpret_cast<u64>(m);

  auto writer = [] {
    L* c = map_shared<L>(false);
    at(c->child_addr).store(reinterpret_cast<u64>(c));
    Big cur;
    while (!c->cell.try_read(cur)) {}  // a restarted single writer: its own last commit is stable
    for (u64 i = cur.a[0] + 1;; ++i) {
      Big b;
      for (auto& x : b.a) { x = i; }
      c->cell.write(b);
    }
  };

  // 1. concurrency: no torn value ever read.
  pid_t pid = spawn(writer);
  u64 ok = 0, retry = 0, torn = 0, last = 0, backwards = 0;
  const auto until = Clock::now() + std::chrono::milliseconds(1500);
  while (Clock::now() < until) {
    Big b;
    if (!m->cell.try_read(b)) {
      ++retry;
      continue;
    }
    ++ok;
    for (auto x : b.a) {
      if (x != b.a[0]) {
        ++torn;
        break;
      }
    }
    if (b.a[0] < last) { ++backwards; }
    last = b.a[0];
  }
  const bool distinct = at(m->child_addr).load() != 0 && at(m->child_addr).load() != m->parent_addr;
  kill_reap(pid);
  printf("%s concurrency: reads ok=%llu retry=%llu torn=%llu backwards=%llu last=%llu; mapped at a different address in the child: %s\n",
         label, (unsigned long long)ok, (unsigned long long)retry, (unsigned long long)torn,
         (unsigned long long)backwards, (unsigned long long)last, distinct ? "yes" : "NO");

  // 2. death: SIGKILL the writer at a random instant, then one read attempt with nobody writing.
  std::mt19937 rng(1);
  int stuck = 0, torn_after = 0, progressed = 0;
  u64 prev = 0;
  for (int k = 0; k < kills; ++k) {
    pid = spawn(writer);
    sleep_us(300 + rng() % 1500);
    kill_reap(pid);
    Big b;
    if (!m->cell.try_read(b)) {
      ++stuck;
      // Nobody is writing, so this never clears by itself. Stand in for a new writer's repair.
      u64& seq = *reinterpret_cast<u64*>(&m->cell);
      at(seq).store((at(seq).load() | 1) + 1);
      for (auto& w : m->cell.word0()) { at(w).store(prev); }
      continue;
    }
    for (auto x : b.a) {
      if (x != b.a[0]) {
        ++torn_after;
        break;
      }
    }
    if (b.a[0] > prev) { ++progressed; }
    prev = b.a[0];
  }
  printf("%s death: %d kills -> reader stuck %d, torn %d, value advanced across %d kills\n", label, kills, stuck,
         torn_after, progressed);
  munmap(m, sizeof(L));
}

// The stuck-reader repair above needs raw access to the payload; keep that out of utx.hpp.
template <class T>
struct SeqCellT : SeqCell<T> {
  std::span<u64> word0() { return {this->word, kWordsFor<T>}; }
};
template <class T>
struct ShadowCellT : ShadowCell<T> {
  std::span<u64> word0() { return {}; }  // never needed: ShadowCell cannot get stuck
};

// ---- C: ring ---------------------------------------------------------------------------------
struct Rec {
  u64 n;
};
static void ring_test(int kills) {
  using Ring = TxRing<4096>;
  Ring* q    = map_shared<Ring>(true);

  // Each transaction is three records {i, 2i, 3i} in one frame of 8 + 24 bytes, so w / 32 is the
  // number of commits so far and a restarted producer knows where it was without any other state.
  auto producer = [] {
    Ring* c = map_shared<Ring>(false);
    for (;;) {
      const u64 i = at(c->w).load() / 32;
      Ring::Tx tx(*c);
      tx.append(Rec{i});
      tx.append(Rec{2 * i});
      tx.append(Rec{3 * i});
      if (!tx.commit()) { cpu_relax(); }  // full: abort, try again
    }
  };

  std::mt19937 rng(2);
  std::byte scratch[256];
  u64 next = 0, torn = 0, gaps = 0, corrupt = 0, redelivered = 0;
  bool refuse_once = true;
  auto drain = [&](std::chrono::microseconds d) {
    const auto until = Clock::now() + d;
    while (Clock::now() < until) {
      const auto t = q->consume(scratch, [&](std::span<const std::byte> f) {
        Rec r[3];
        if (f.size() != sizeof r) { return ++torn, true; }
        std::memcpy(r, f.data(), sizeof r);
        if (r[1].n != 2 * r[0].n || r[2].n != 3 * r[0].n) { ++torn; }
        if (refuse_once && r[0].n == 1000) {  // "died before the ack": the frame must come back
          refuse_once = false;
          return false;
        }
        if (r[0].n == 1000 && !refuse_once) { ++redelivered; }
        if (r[0].n != next) { ++gaps; }
        next = r[0].n + 1;
        return true;
      });
      if (t == Ring::Take::kCorrupt || t == Ring::Take::kTooBig) { ++corrupt; }
    }
  };
  for (int k = 0; k < kills; ++k) {
    const pid_t pid = spawn(producer);
    drain(std::chrono::microseconds(300 + rng() % 1500));
    kill_reap(pid);
  }
  drain(std::chrono::microseconds(2000));
  printf("ring: %d producer kills -> frames=%llu torn=%llu gaps_or_dups=%llu corrupt=%llu redelivered_after_refused_ack=%llu\n",
         kills, (unsigned long long)next, (unsigned long long)torn, (unsigned long long)gaps,
         (unsigned long long)corrupt, (unsigned long long)redelivered);

  // A hostile length: the consumer must refuse it, not read past the committed bytes.
  const u64 rd = at(q->r).load(), bogus = ~u64{0};
  q->put(rd, &bogus, 8);
  at(q->w).store(rd + 32);
  const auto t = q->consume(scratch, [](std::span<const std::byte>) { return true; });
  printf("ring: frame claiming 2^64-1 bytes -> %s\n", t == Ring::Take::kCorrupt ? "refused (kCorrupt)" : "ACCEPTED");
  munmap(q, sizeof(Ring));
}

// ---- D: region -------------------------------------------------------------------------------
enum { kAccounts = 16, kCommits = 16, kSteals, kReplays, kLost, kWords };
using Region = TxRegion<kWords, 4>;

// Impure verdict, slow path only: ask the kernel whether the owner's pid still exists.
struct PidDead {
  bool operator()(u64 tok, unsigned spins) const noexcept {
    return spins % 256 == 255 && kill(static_cast<pid_t>(tok >> 32), 0) == -1 && errno == ESRCH;
  }
};

template <class Dead>
static void transfer_loop(Region* g, bool logged, Dead dead_proto) {
  std::mt19937_64 rng(static_cast<u64>(getpid()) * 0x9E3779B97F4A7C15ull);
  const u32 self = static_cast<u32>(getpid());
  for (;;) {
    const std::size_t a = rng() % kAccounts, b = (a + 1 + rng() % (kAccounts - 1)) % kAccounts;
    const u64 amt = rng() % 50;
    Dead dead = dead_proto;
    bool stole = false, replayed = false;
    if (logged) {
      Region::Tx tx(*g, self, dead, &stole, &replayed);
      tx.write(a, tx.read(a) - amt);
      tx.write(b, tx.read(b) + amt);
      tx.write(kCommits, tx.read(kCommits) + 1);
      if (!tx.commit()) { at(g->word[kLost]).fetch_add(1); }
    } else {  // the same lock and the same theft, but the words are written in place, with no log
      const u64 tok = g->acquire(self, dead, &stole, &replayed);
      at(g->word[a]).store(at(g->word[a]).load() - amt);
      at(g->word[b]).store(at(g->word[b]).load() + amt);
      at(g->word[kCommits]).fetch_add(1);
      g->release(tok);
    }
    // Diagnostics only, outside any transaction.
    if (stole) { at(g->word[kSteals]).fetch_add(1); }
    if (replayed) { at(g->word[kReplays]).fetch_add(1); }
  }
}

template <class Dead>
static void region_test(const char* label, bool logged, Dead dead, int kills, int run_ms, int stop_us = 0,
                        int kill_every = 1) {
  Region* g = map_shared<Region>(true);
  for (int i = 0; i < kAccounts; ++i) { g->word[i] = 1000; }
  auto writer = [=] { transfer_loop(map_shared<Region>(false), logged, dead); };
  pid_t pids[2] = {spawn(writer), spawn(writer)};

  std::mt19937 rng(3);
  u64 snaps = 0, bad = 0, retries = 0;
  auto check = [&] {
    u64 s[kWords];
    if (!g->try_snapshot(s)) { return ++retries, false; }
    u64 sum = 0;
    for (int i = 0; i < kAccounts; ++i) { sum += s[i]; }
    ++snaps;
    if (sum != 1000u * kAccounts) { ++bad; }
    return true;
  };
  const auto until = Clock::now() + std::chrono::milliseconds(run_ms);
  int done = 0;
  for (int it = 1; Clock::now() < until; ++it) {
    for (int i = 0; i < 50; ++i) { check(); }
    sleep_us(200 + rng() % 800);
    if (done < kills && it % kill_every == 0) {
      const int v = rng() % 2;
      kill_reap(pids[v]);
      pids[v] = spawn(writer);
      ++done;
    } else if (stop_us && done >= kills) {  // freeze a live writer for longer than the lease, then let it run on
      const int v = rng() % 2;
      kill(pids[v], SIGSTOP);
      sleep_us(stop_us);
      kill(pids[v], SIGCONT);
    }
  }
  kill_reap(pids[0]);
  kill_reap(pids[1]);
  g->recover(static_cast<u32>(getpid()), PidDead{});  // both owners are reaped: this is safe
  u64 s[kWords];
  const bool final_ok = g->try_snapshot(s);
  u64 sum = 0;
  for (int i = 0; i < kAccounts; ++i) { sum += s[i]; }
  printf("%s: kills=%d commits=%llu steals=%llu replays=%llu lost_lease=%llu | snapshots=%llu bad=%llu retries=%llu | final sum=%llu (%s)\n",
         label, done, (unsigned long long)s[kCommits], (unsigned long long)s[kSteals], (unsigned long long)s[kReplays],
         (unsigned long long)s[kLost], (unsigned long long)snaps, (unsigned long long)bad, (unsigned long long)retries,
         (unsigned long long)sum, final_ok && sum == 1000u * kAccounts ? "conserved" : "BROKEN");
  munmap(g, sizeof(Region));
}

// ---- timings ---------------------------------------------------------------------------------
struct Small {
  u64 a[8];
};
template <class F>
static double ns_per(F&& f, int n = 5'000'000) {
  const auto t0 = Clock::now();
  for (int i = 0; i < n; ++i) { f(i); }
  return std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / n;
}
static void bench() {
  struct L {
    SeqCell<Small> seq;
    ShadowCell<Small> shadow;
    TxRing<4096> ring;
    Region region;
    pthread_mutex_t mu;
    u64 plain[2];
  };
  L* m = map_shared<L>(true);
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&m->mu, &attr);
  Small v{}, out{};
  u64 sink = 0;
  std::byte scratch[64];
  printf("uncontended, one process, ns per operation (64-byte payload):\n");
  printf("  SeqCell     write %.1f  read %.1f\n", ns_per([&](int i) { v.a[0] = i, m->seq.write(v); }),
         ns_per([&](int) { sink += m->seq.try_read(out); }));
  printf("  ShadowCell  write %.1f  read %.1f\n", ns_per([&](int i) { v.a[0] = i, m->shadow.write(v); }),
         ns_per([&](int) { sink += m->shadow.try_read(out); }));
  printf("  TxRing      commit(1 record)+consume %.1f\n", ns_per([&](int) {
           TxRing<4096>::Tx tx(m->ring);
           tx.append(v);
           tx.commit();
           m->ring.consume(scratch, [&](std::span<const std::byte> f) { return sink += f.size(), true; });
         }));
  const u32 self = static_cast<u32>(getpid());
  printf("  TxRegion    3-word transaction %.1f\n", ns_per([&](int i) {
           Region::Tx tx(m->region, self, PidDead{});
           tx.write(0, i), tx.write(1, i), tx.write(2, i);
           sink += tx.commit();
         }));
  printf("  robust process-shared pthread_mutex, lock + 3 stores + unlock %.1f\n", ns_per([&](int i) {
           pthread_mutex_lock(&m->mu);
           m->plain[0] = i, m->plain[1] = i, sink += i;
           pthread_mutex_unlock(&m->mu);
         }));
  if (sink == 42) { puts(""); }
  munmap(m, sizeof(L));
}

int main(int argc, char** argv) {
  g_name = "/utx-test-" + std::to_string(getpid());
  const std::string what = argc > 1 ? argv[1] : "all";
  const auto is = [&](const char* s) { return what == "all" || what == s; };
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (is("seq")) { cell_test<SeqCellT>("SeqCell", 300); }
  if (is("shadow")) { cell_test<ShadowCellT>("ShadowCell", 300); }
  if (is("ring")) { ring_test(300); }
  if (is("region")) {
    region_test("TxRegion  redo log, pid verdict ", true, PidDead{}, 400, 3000);
    region_test("in-place writes, pid verdict    ", false, PidDead{}, 400, 3000);
    region_test("TxRegion  redo log, 50 ms lease ", true, Lease{std::chrono::milliseconds(50)}, 60, 8000, 0, 150);
    region_test("TxRegion  5 ms lease, 20 ms stops", true, Lease{std::chrono::milliseconds(5)}, 0, 3000, 20000);
  }
  if (is("bench")) { bench(); }
  shm_unlink(g_name.c_str());
  return 0;
}
