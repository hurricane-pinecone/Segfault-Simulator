#include "engine/core/util/debugFeatures.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

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

// logEvents()'s list, from SFS_LOG_EVENTS until setLogEvents() replaces it.
struct EventLogList
{
  EventLogList()
  {
    if (const char* list = std::getenv("SFS_LOG_EVENTS"))
      types = list;
  }

  std::mutex mutex;
  std::string types;
};

EventLogList& eventLogList()
{
  static EventLogList list;
  return list;
}
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

bool logEvents(std::string_view typeName)
{
  if (!enabled())
    return false;

  EventLogList& list = eventLogList();
  const std::lock_guard lock(list.mutex);
  std::string_view rest = list.types;

  while (!rest.empty())
  {
    const std::size_t comma = rest.find(',');
    std::string_view item = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view()
                                           : rest.substr(comma + 1);

    while (!item.empty() && item.front() == ' ')
      item.remove_prefix(1);

    while (!item.empty() && item.back() == ' ')
      item.remove_suffix(1);

    if (item.empty())
      continue;

    // A name without namespaces matches the end of a qualified one.
    if (item == "*" || item == typeName ||
        (typeName.ends_with(item) &&
         typeName.substr(0, typeName.size() - item.size()).ends_with("::")))
      return true;
  }

  return false;
}

void setLogEvents(std::string_view types)
{
  EventLogList& list = eventLogList();
  const std::lock_guard lock(list.mutex);
  list.types = types;
}

} // namespace sfs::debugFeatures
