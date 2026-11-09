#include "engine/core/util/cpuTiming.h"

#include "engine/core/logger/logger.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <typeinfo>
#include <vector>
#if defined(__GNUC__) || defined(__clang__)
  #include <cxxabi.h>
#endif

namespace sfs::cpuTiming
{

namespace
{

struct Row
{
  const char* name;
  double total = 0.0; // milliseconds
  uint64_t calls = 0;
  double frame = 0.0; // milliseconds this frame (for the slow frame lines)
};

struct State
{
  std::mutex mutex;
  std::vector<Row> rows; // in the order first seen
  std::deque<std::string> labels;
  struct Typed
  {
    const char* prefix;
    const std::type_info* type;
    const char* label;
  };
  std::vector<Typed> typed; // label()s of types, found again quickly
  uint64_t frames = 0;
  std::chrono::steady_clock::time_point since =
      std::chrono::steady_clock::now();
  const std::chrono::steady_clock::time_point started = since;
  std::chrono::steady_clock::time_point frameEnded = since;
  uint64_t frameCount = 0; // every frame since the start
};

// SFS_SLOW_FRAME_MS=<ms>: a frame longer than that (end to end) logs its
// biggest scopes, to find what a hitch was. 0: off.
double slowFrameMs()
{
  static const double ms = []
  {
    const char* value = std::getenv("SFS_SLOW_FRAME_MS");
    return value ? std::max(0.0, std::atof(value)) : 0.0;
  }();
  return ms;
}

bool periodic()
{
  static const bool on = std::getenv("SFS_CPU_TIMING") != nullptr;
  return on;
}

State& state()
{
  static State* s = new State(); // never destroyed: scopes may outlive main
  return *s;
}

std::string readable(const std::type_info& type)
{
  std::string name = type.name();
#if defined(__GNUC__) || defined(__clang__)
  int status = 0;
  char* demangled =
      abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
  if (status == 0 && demangled)
    name = demangled;
  std::free(demangled);
#endif
  if (name.rfind("sfs::", 0) == 0)
    name = name.substr(5);
  return name;
}

} // namespace

bool enabled()
{
  static const bool on = periodic() || slowFrameMs() > 0.0;
  return on;
}

void add(const char* name, double milliseconds)
{
  State& s = state();
  const std::lock_guard lock(s.mutex);
  for (Row& row : s.rows)
    if (row.name == name || std::strcmp(row.name, name) == 0)
    {
      row.total += milliseconds;
      row.calls += 1;
      row.frame += milliseconds;
      return;
    }
  s.rows.push_back({name, milliseconds, 1, milliseconds});
}

const char* label(const char* prefix, const std::string& name)
{
  State& s = state();
  const std::lock_guard lock(s.mutex);
  const std::string full = std::string(prefix) + name;
  for (const std::string& existing : s.labels)
    if (existing == full)
      return existing.c_str();
  return s.labels.emplace_back(full).c_str();
}

const char* label(const char* prefix, const std::type_info& type)
{
  State& s = state();
  {
    const std::lock_guard lock(s.mutex);
    for (const State::Typed& known : s.typed)
      if (known.prefix == prefix && *known.type == type)
        return known.label;
  }
  const char* found = label(prefix, readable(type));
  const std::lock_guard lock(s.mutex);
  s.typed.push_back({prefix, &type, found});
  return found;
}

void endFrame()
{
  if (!enabled())
    return;
  State& s = state();
  const std::lock_guard lock(s.mutex);
  s.frames += 1;
  s.frameCount += 1;
  const auto now = std::chrono::steady_clock::now();
  const double frameMs =
      std::chrono::duration<double, std::milli>(now - s.frameEnded).count();
  s.frameEnded = now;
  if (slowFrameMs() > 0.0 && frameMs > slowFrameMs())
  {
    std::vector<const Row*> biggest;
    for (const Row& row : s.rows)
      if (row.frame >= 0.25)
        biggest.push_back(&row);
    std::sort(biggest.begin(),
              biggest.end(),
              [](const Row* a, const Row* b) { return a->frame > b->frame; });
    char part[160];
    std::snprintf(part,
                  sizeof(part),
                  "Slow frame %llu at %.2f s: %.1f ms:",
                  static_cast<unsigned long long>(s.frameCount),
                  std::chrono::duration<double>(now - s.started).count(),
                  frameMs);
    std::string line = part;
    for (std::size_t i = 0; i < biggest.size() && i < 16; ++i)
    {
      std::snprintf(
          part, sizeof(part), " %s %.2f;", biggest[i]->name, biggest[i]->frame);
      line += part;
    }
    LOG_INFO(line);
  }
  for (Row& row : s.rows)
    row.frame = 0.0;
  if (!periodic())
    return;
  const double seconds = std::chrono::duration<double>(now - s.since).count();
  if (seconds < 5.0)
    return;
  const double frames = static_cast<double>(s.frames);
  char part[160];
  std::snprintf(part,
                sizeof(part),
                "CPU ms/frame over %llu frames (%.1f fps, %.3f ms):",
                static_cast<unsigned long long>(s.frames),
                frames / seconds,
                seconds * 1000.0 / frames);
  std::string line = part;
  for (const Row& row : s.rows)
  {
    // Calls a frame, where not once.
    const double calls = static_cast<double>(row.calls) / frames;
    if (calls > 0.99 && calls < 1.01)
      std::snprintf(
          part, sizeof(part), " %s %.3f;", row.name, row.total / frames);
    else
      std::snprintf(part,
                    sizeof(part),
                    " %s %.3f (x%.2f);",
                    row.name,
                    row.total / frames,
                    calls);
    line += part;
  }
  LOG_INFO(line);
  s.rows.clear();
  s.frames = 0;
  s.since = now;
}

} // namespace sfs::cpuTiming
