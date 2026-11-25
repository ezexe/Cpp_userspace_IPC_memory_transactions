#include "../utx.hpp"
#include <thread>
#include <cstdio>
using namespace utx;
struct Small { u64 a[8]; };
static SeqCell<Small> seq; static ShadowCell<Small> shadow; static TxRing<4096> ring; static TxRegion<8,4> region;
int main() {
  std::atomic<bool> stop{false};
  std::thread w([&]{ Small v{}; for (u64 i = 0; i < 200000; ++i) { v.a[0]=v.a[7]=i; seq.write(v); shadow.write(v);
      TxRing<4096>::Tx tx(ring); tx.append(v); tx.commit(); } stop = true; });
  std::thread w2([&]{ for (int i = 0; i < 100000; ++i) { TxRegion<8,4>::Tx tx(region, 2, Lease{std::chrono::seconds(60)}); tx.write(0, tx.read(0)+1); tx.write(1, tx.read(1)-1); tx.commit(); } });
  std::thread w3([&]{ for (int i = 0; i < 100000; ++i) { TxRegion<8,4>::Tx tx(region, 3, Lease{std::chrono::seconds(60)}); tx.write(0, tx.read(0)+1); tx.write(1, tx.read(1)-1); tx.commit(); } });
  u64 bad = 0, n = 0; std::byte scratch[128];
  while (!stop) { Small o; if (seq.try_read(o)) { bad += o.a[0]!=o.a[7]; ++n; } if (shadow.try_read(o)) { bad += o.a[0]!=o.a[7]; }
    ring.consume(scratch, [](std::span<const std::byte>){ return true; });
    u64 s[8]; if (region.try_snapshot(s)) { bad += (s[0] + s[1]) != 0; } }
  w.join(); w2.join(); w3.join();
  printf("tsan run: reads=%llu bad=%llu region word0=%llu\n", (unsigned long long)n, (unsigned long long)bad, (unsigned long long)region.word[0]);
}
