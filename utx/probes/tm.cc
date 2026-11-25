#include <cstdio>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
int main(int argc, char**) {
  long* p = static_cast<long*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  auto body = [p] { for (int i = 0; i < 2000000; ++i) { __transaction_atomic { p[0] = p[0] + 1; p[1] = p[1] - 1; } } };
  if (argc > 1) { std::thread a(body), b(body); a.join(); b.join(); printf("two threads   x 2e6: p0=%ld p1=%ld\n", p[0], p[1]); return 0; }
  for (int who = 0; who < 2; ++who) if (fork() == 0) { body(); _exit(0); }
  wait(nullptr); wait(nullptr);
  printf("two processes x 2e6: p0=%ld p1=%ld\n", p[0], p[1]);
}
