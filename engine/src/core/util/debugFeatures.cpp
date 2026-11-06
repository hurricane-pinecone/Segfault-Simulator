#include "engine/core/util/debugFeatures.h"

#include <atomic>

namespace sfs::debugFeatures
{

namespace
{
// A Debug build is for making the game: allowed from the start.
#ifdef NDEBUG
std::atomic<bool> g_allowed{false};
#else
std::atomic<bool> g_allowed{true};
#endif
} // namespace

void allow(bool allowed)
{
  g_allowed.store(allowed, std::memory_order_relaxed);
}

bool enabled()
{
  return compiled() && g_allowed.load(std::memory_order_relaxed);
}

} // namespace sfs::debugFeatures
