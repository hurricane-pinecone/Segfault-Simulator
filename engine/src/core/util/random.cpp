#include "engine/core/util/random.h"

#include "engine/core/util/hash.h"

#include <atomic>
#include <random>

namespace sfs::random
{

namespace
{
// splitmix64's increment: every state is visited once before any repeats.
constexpr uint64_t kGamma = 0x9E3779B97F4A7C15ull;

std::atomic<uint64_t>& state()
{
  static std::atomic<uint64_t> value{
      std::random_device{}() ^
      (static_cast<uint64_t>(std::random_device{}()) << 32)};
  return value;
}
} // namespace

uint64_t next()
{
  return mix64(state().fetch_add(kGamma, std::memory_order_relaxed) + kGamma);
}

void seed(uint64_t seed) { state().store(seed, std::memory_order_relaxed); }

} // namespace sfs::random
