#include "../utx.hpp"
#include <cstdio>
#include <cstddef>
using namespace utx;
struct Big { u64 a[512]; };
int main() {
  printf("ShadowCell<Big>: ver@%zu slot@%zu size=%zu | SeqCell<Big>: seq@%zu word@%zu | TxRing<4096>: w@%zu r@%zu buf@%zu size=%zu\n",
    offsetof(ShadowCell<Big>, ver), offsetof(ShadowCell<Big>, slot), sizeof(ShadowCell<Big>),
    offsetof(SeqCell<Big>, seq), offsetof(SeqCell<Big>, word),
    offsetof(TxRing<4096>, w), offsetof(TxRing<4096>, r), offsetof(TxRing<4096>, buf), sizeof(TxRing<4096>));
  using R = TxRegion<8,4>;
  printf("TxRegion<8,4>: lock@%zu ticket@%zu seq@%zu committed@%zu log@%zu word@%zu size=%zu; standard_layout=%d trivially_copyable=%d trivially_default_constructible=%d\n",
    offsetof(R, lock), offsetof(R, ticket), offsetof(R, seq), offsetof(R, committed), offsetof(R, log), offsetof(R, word), sizeof(R),
    std::is_standard_layout_v<R>, std::is_trivially_copyable_v<R>, std::is_trivially_default_constructible_v<R>);
}
