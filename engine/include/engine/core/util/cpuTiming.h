#pragma once

#include <chrono>
#include <string>
#include <typeinfo>

namespace sfs::cpuTiming
{

// Timing what the CPU does each frame, for measuring what it costs: with
// SFS_CPU_TIMING set, each scope's time (milliseconds a frame, and calls)
// is logged every few seconds. Scopes nest (a scope's time includes those
// inside it); same-named scopes add up. Off, a scope costs a branch.
// SFS_SLOW_FRAME_MS=<ms> (with or without SFS_CPU_TIMING): each frame longer
// than that logs a "Slow frame" line with its biggest scopes.

bool enabled();

// Adds a scope's time. `name` must outlive the program (a literal, or one
// from label()).
void add(const char* name, double milliseconds);

// A lasting copy of `prefix` + `name`, or + the type's readable name.
const char* label(const char* prefix, const std::string& name);
const char* label(const char* prefix, const std::type_info& type);

// At the end of each frame: logs the averages every few seconds.
void endFrame();

class Scope
{
public:
  explicit Scope(const char* name) : m_name(enabled() ? name : nullptr)
  {
    if (m_name)
      m_start = std::chrono::steady_clock::now();
  }
  ~Scope()
  {
    if (m_name)
      add(m_name,
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - m_start)
              .count());
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  const char* m_name;
  std::chrono::steady_clock::time_point m_start;
};

} // namespace sfs::cpuTiming

#define SFS_CPU_SCOPE_JOIN2(a, b) a##b
#define SFS_CPU_SCOPE_JOIN(a, b) SFS_CPU_SCOPE_JOIN2(a, b)
// Times the rest of the enclosing block under `name`.
#define SFS_CPU_SCOPE(name)                                                    \
  const ::sfs::cpuTiming::Scope SFS_CPU_SCOPE_JOIN(sfsCpuScope, __LINE__)(name)
