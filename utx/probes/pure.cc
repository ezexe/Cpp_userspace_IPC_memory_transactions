// Proof that the hot paths make no system call: after mapping, each child installs a seccomp filter
// that kills the process on any system call but exit, then runs every primitive under contention
// with its sibling.
#include "../utx.hpp"
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <cstddef>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
using namespace utx;
struct Small { u64 a[8]; };
struct L {
  SeqCell<Small> seq; ShadowCell<Small> shadow; TxRing<4096> ring; TxRegion<8, 4> region;
  u64 lease_checks;
};
int main() {
  L* m = static_cast<L*>(mmap(nullptr, sizeof(L), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  (void)std::chrono::steady_clock::now();  // touch the vDSO before the filter
  pid_t pid[2];
  for (int who = 0; who < 2; ++who) {
    if ((pid[who] = fork()) == 0) {
      // Filter mode, not strict mode: strict also turns RDTSC into SIGSEGV, which kills the vDSO clock.
      sock_filter f[] = {
          BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit, 0, 1),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      };
      sock_fprog prog{sizeof f / sizeof f[0], f};
      if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) { _exit(3); }
      std::byte scratch[128];
      Small v{}, out{};
      u64 sink = 0;
      for (u64 i = 0; i < 2'000'000; ++i) {
        if (who == 0) {
          v.a[0] = i; m->seq.write(v); m->shadow.write(v);
          TxRing<4096>::Tx tx(m->ring); tx.append(v); tx.commit();
        } else {
          sink += m->seq.try_read(out) + m->shadow.try_read(out);
          m->ring.consume(scratch, [&](std::span<const std::byte> f) { return sink += f.size(), true; });
        }
        struct Counting : Lease {
          u64* n;
          bool operator()(u64 cur, unsigned spins) noexcept {
            if (spins % 256 == 0) { at(*n).fetch_add(1); }
            return Lease::operator()(cur, spins);
          }
        } dead{{std::chrono::seconds(10)}, &m->lease_checks};
        TxRegion<8, 4>::Tx tx(m->region, static_cast<u32>(who + 1), dead);
        tx.write(0, tx.read(0) + 1); tx.write(1, tx.read(1) - 1);
        sink += tx.commit();
      }
      syscall(SYS_exit, sink ? 0 : 0);
    }
  }
  int ok = 0;
  for (int who = 0; who < 2; ++who) {
    int st; waitpid(pid[who], &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { ++ok; }
    else { printf("child %d: %s %d\n", who, WIFSIGNALED(st) ? "killed by signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st)); }
  }
  printf("seccomp (only exit allowed): %d/2 children finished; region word0=%llu word1=%lld; clock reads by the lease under contention=%llu\n",
         ok, (unsigned long long)m->region.word[0], (long long)m->region.word[1], (unsigned long long)m->lease_checks);
}
