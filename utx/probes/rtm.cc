#include <immintrin.h>
#include <cpuid.h>
#include <cstdio>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
int main() {
  unsigned a, b, c, d; __cpuid_count(7, 0, a, b, c, d);
  printf("cpuid.7.0: RTM=%u HLE=%u; edx RTM_ALWAYS_ABORT=%u\n", (b >> 11) & 1, (b >> 4) & 1, (d >> 11) & 1);
  if (!((b >> 11) & 1)) return 0;
  long* p = static_cast<long*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  // p[0], p[1]: the pair; p[2]: fallback spinlock; p[8..]: per-process stats
  for (int who = 0; who < 2; ++who) if (fork() == 0) {
    long ok = 0, fb = 0;
    for (int i = 0; i < 2000000; ++i) {
      int tries = 0; bool done = false;
      while (tries++ < 8 && !done) {
        unsigned st = _xbegin();
        if (st == _XBEGIN_STARTED) {
          if (__atomic_load_n(&p[2], __ATOMIC_RELAXED)) _xabort(1);   // subscribe to the fallback lock
          p[0]++; p[1]--; _xend(); done = true; ++ok;
        }
      }
      if (!done) { long e = 0; while (!__atomic_compare_exchange_n(&p[2], &e, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { e = 0; _mm_pause(); }
        p[0]++; p[1]--; __atomic_store_n(&p[2], 0, __ATOMIC_RELEASE); ++fb; }
    }
    p[8 + who * 2] = ok; p[9 + who * 2] = fb; _exit(0);
  }
  wait(nullptr); wait(nullptr);
  printf("two processes x 2e6: p0=%ld p1=%ld | hardware commits=%ld fallback=%ld\n", p[0], p[1], p[8] + p[10], p[9] + p[11]);
}
