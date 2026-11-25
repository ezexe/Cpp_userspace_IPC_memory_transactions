#include <memory>
#include <atomic>
#include <cstdint>
#include <type_traits>
struct Pod { std::uint64_t a; };
struct WithAtomic { std::atomic<std::uint64_t> a; };
static_assert(std::is_trivially_copyable_v<Pod>);
#ifdef TRY_SLA
void* f(void* p) { return std::start_lifetime_as<Pod>(p); }
#endif
#ifdef TRY_CONST
std::uint64_t g(const std::uint64_t& x) { return std::atomic_ref<const std::uint64_t>(x).load(); }
#endif
#ifdef TRY_TC
static_assert(std::is_trivially_copyable_v<WithAtomic>);
static_assert(std::is_trivially_default_constructible_v<WithAtomic>);
#endif
int main() {}
