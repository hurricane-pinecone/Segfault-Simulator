#include "engine/core/util/debugFeatures.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

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

bool envSet(const char* name)
{
  const char* value = std::getenv(name);
  return value && std::strcmp(value, "0") != 0 && value[0] != '\0';
}

std::atomic<bool> g_checkTransforms{envSet("SFS_CHECK_TRANSFORMS")};
} // namespace

void allow(bool allowed)
{
  g_allowed.store(allowed, std::memory_order_relaxed);
}

bool enabled()
{
  return compiled() && g_allowed.load(std::memory_order_relaxed);
}

bool checkTransforms()
{
  return enabled() && g_checkTransforms.load(std::memory_order_relaxed);
}

void setCheckTransforms(bool on)
{
  g_checkTransforms.store(on, std::memory_order_relaxed);
}

} // namespace sfs::debugFeatures
