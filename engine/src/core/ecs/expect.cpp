#include "engine/core/ecs/expect.h"

#include "engine/core/ecs/registry.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/core/util/errorDialog.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif defined(__linux__)
  #include <csignal>
  #include <fstream>
#endif

namespace sfs::detail
{

namespace
{

// Whether a debugger is attached to the process: Windows asks, Linux reads
// the tracer's pid from /proc. Elsewhere none is ever seen.
bool debuggerAttached()
{
#if defined(_WIN32)
  return IsDebuggerPresent() != 0;
#elif defined(__linux__)
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line))
    if (line.rfind("TracerPid:", 0) == 0)
      return std::atoi(line.c_str() + std::strlen("TracerPid:")) != 0;
  return false;
#else
  return false;
#endif
}

void breakIntoDebugger()
{
#if defined(_WIN32)
  DebugBreak();
#elif defined(__linux__)
  std::raise(SIGTRAP);
#endif
}

bool envSet(const char* name)
{
  const char* value = std::getenv(name);
  return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

// A call site, by its source_location (a file's name is one string per
// translation unit, so a header's call site counts once in each that uses it).
struct Site
{
  const char* file;
  uint_least32_t line;
  uint_least32_t column;
  bool operator==(const Site&) const = default;
};

struct SiteHash
{
  std::size_t operator()(const Site& site) const noexcept
  {
    return std::hash<const void*>()(site.file) ^
           (std::size_t(site.line) * 0x9E3779B97F4A7C15ull) ^ site.column;
  }
};

// The call sites that have failed once already this run.
std::mutex g_sitesMutex;
std::unordered_set<Site, SiteHash> g_failedSites;

// Whether `where` fails for the first time this run (it's remembered).
bool firstFailure(const std::source_location& where)
{
  const std::lock_guard lock(g_sitesMutex);
  return g_failedSites.insert({where.file_name(), where.line(), where.column()})
      .second;
}

} // namespace

void expectFailed(const std::source_location& where,
                  const Entity& entity,
                  std::string_view type)
{
  if (!firstFailure(where))
    return;
  std::string_view file = where.file_name();
  if (const std::size_t slash = file.find_last_of("/\\");
      slash != std::string_view::npos)
    file.remove_prefix(slash + 1);

  const std::string who = entity.getId() == Entity::InvalidId
                              ? std::string("an empty entity handle")
                              : "entity " + std::to_string(entity.getId());
  std::string message = std::string(file) + ":" + std::to_string(where.line()) +
                        ": " + who + " has no " + readableTypeName(type);
  if (!entity.isValid() && entity.getId() != Entity::InvalidId)
    message += " (the handle isn't a living entity)";
  LOG_ERROR(message);

  // Release: the log line and the fallback, nothing more.
  if (!debugFeatures::enabled())
    return;
  if (envSet("SFS_EXPECT_ABORT"))
  {
    Logger::flush();
    std::fprintf(stderr, "SFS_EXPECT_ABORT: %s\n", message.c_str());
    std::abort();
  }
  if (debuggerAttached())
    breakIntoDebugger();
  showErrorDialog(message +
                  "\n\nThe game carries on without it. The log has the "
                  "details.");
}

void forgetExpectFailures()
{
  const std::lock_guard lock(g_sitesMutex);
  g_failedSites.clear();
}

} // namespace sfs::detail
