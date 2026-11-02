#include <engine/core/logger/logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace sfs
{

#ifdef NDEBUG
Logger::Level Logger::s_logLevel = Logger::Level::INFO;
#else
Logger::Level Logger::s_logLevel = Logger::Level::DEBUG;
#endif
Logger::Verbosity Logger::verbosity = Logger::Verbosity::MINIMAL;

namespace
{
// Sinks are called outside the lock, from a copy of the list: a sink may
// log, or add and remove sinks, without deadlocking. Each counts the calls
// under way, so removing one waits for them (what it writes to may go next).
struct SinkEntry
{
  explicit SinkEntry(Logger::Sink sink) : sink(std::move(sink)) {}
  Logger::Sink sink;
  std::atomic<int> calls{0};
};

struct SinkRegistry
{
  std::mutex mutex;
  std::vector<std::pair<Logger::SinkId, std::shared_ptr<SinkEntry>>> sinks;
  Logger::SinkId nextId = 1;
};

// Set on a thread while it's in a sink: what that sink logs goes to the
// console only, not back into the sinks (which could go on forever).
thread_local bool t_inSink = false;

// Function-local so it exists before any static-initialisation-time log.
SinkRegistry& sinkRegistry()
{
  static SinkRegistry registry;
  return registry;
}

// The last message logged, to count repeats of it rather than write them.
struct LastMessage
{
  std::mutex mutex;
  Logger::Level level = Logger::Level::DISABLED;
  std::string message;
  const char* file = "";
  int line = 0;
  const char* func = "";
  std::size_t repeats = 0; // not yet written
  std::chrono::steady_clock::time_point written;
};

LastMessage& lastMessage()
{
  static LastMessage last;
  return last;
}

// A message logged again within this long of being written is counted, not
// written: one logged every frame shows once a second (with the count of the
// ones in between), and the same message a while later shows as usual.
constexpr std::chrono::seconds kRepeatWindow{1};
} // namespace

Logger::SinkId Logger::addSink(Sink sink)
{
  SinkRegistry& registry = sinkRegistry();
  const std::lock_guard lock(registry.mutex);
  const SinkId id = registry.nextId++;
  registry.sinks.emplace_back(id, std::make_shared<SinkEntry>(std::move(sink)));
  return id;
}

void Logger::removeSink(SinkId id)
{
  SinkRegistry& registry = sinkRegistry();
  std::shared_ptr<SinkEntry> removed;
  {
    const std::lock_guard lock(registry.mutex);
    const auto it =
        std::find_if(registry.sinks.begin(),
                     registry.sinks.end(),
                     [id](const auto& entry) { return entry.first == id; });
    if (it == registry.sinks.end())
      return;
    removed = std::move(it->second);
    registry.sinks.erase(it);
  }
  // Calls under way on other threads finish first. (From inside a sink, this
  // thread's own call would never finish: then it doesn't wait.)
  if (!t_inSink)
    while (removed->calls.load() > 0)
      std::this_thread::yield();
}

void Logger::notifySinks(Level level, const std::string& message)
{
  if (t_inSink)
    return;
  SinkRegistry& registry = sinkRegistry();
  std::vector<std::shared_ptr<SinkEntry>> sinks;
  {
    const std::lock_guard lock(registry.mutex);
    if (registry.sinks.empty())
      return;
    sinks.reserve(registry.sinks.size());
    for (const auto& [id, entry] : registry.sinks)
    {
      ++entry->calls;
      sinks.push_back(entry);
    }
  }
  t_inSink = true;
  for (const std::shared_ptr<SinkEntry>& entry : sinks)
  {
    try
    {
      entry->sink(level, message);
    }
    catch (...)
    {
      // A sink that fails mustn't take the program (or the others) down.
    }
    --entry->calls;
  }
  t_inSink = false;
}

void Logger::info(const std::string& message,
                  const char* file,
                  int line,
                  const char* func)
{
  if (Logger::s_logLevel <= Logger::Level::INFO)
    log(Logger::Level::INFO, message, file, line, func);
}

void Logger::warn(const std::string& message,
                  const char* file,
                  int line,
                  const char* func)
{
  if (Logger::s_logLevel <= Logger::Level::WARN)
    log(Logger::Level::WARN, message, file, line, func);
}

void Logger::error(const std::string& message,
                   const char* file,
                   int line,
                   const char* func)
{
  if (Logger::s_logLevel <= Logger::Level::ERROR)
    log(Logger::Level::ERROR, message, file, line, func);
}

void Logger::debug(const std::string& message,
                   const char* file,
                   int line,
                   const char* func)
{
  if (Logger::s_logLevel <= Logger::Level::DEBUG)
    log(Logger::Level::DEBUG, message, file, line, func);
}

namespace
{
std::string repeatNote(std::size_t repeats)
{
  return " (repeated " + std::to_string(repeats) + " more time" +
         (repeats == 1 ? ")" : "s)");
}
} // namespace

void Logger::log(Level level,
                 const std::string& message,
                 const char* file,
                 int line,
                 const char* func)
{
  // What to write is worked out under the lock, and written after it (a sink
  // may log in turn).
  std::string repeats; // a repeat count to write first, if any
  Level repeatsLevel = level;
  {
    LastMessage& last = lastMessage();
    const std::lock_guard lock(last.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (level == last.level && message == last.message &&
        now - last.written < kRepeatWindow)
    {
      ++last.repeats;
      return;
    }
    if (last.repeats > 0)
      repeats = formatMessage(last.level,
                              last.message + repeatNote(last.repeats),
                              last.file,
                              last.line,
                              last.func);
    repeatsLevel = last.level;
    last.level = level;
    last.message = message;
    last.file = file;
    last.line = line;
    last.func = func;
    last.repeats = 0;
    last.written = now;
  }
  if (!repeats.empty())
    write(repeatsLevel, repeats);
  write(level, formatMessage(level, message, file, line, func));
}

void Logger::write(Level level, const std::string& formatted)
{
  switch (level)
  {
  case Level::ERROR:
    std::cerr << RED << formatted << RESET << std::endl; // flushed
    break;
  case Level::WARN:
    std::cerr << YELLOW << formatted << RESET << std::endl;
    break;
  case Level::DEBUG:
    std::cout << BLUE << formatted << RESET << '\n';
    break;
  default:
    std::cout << formatted << '\n';
    break;
  }
  // Warnings and errors may come just before a crash: whatever led up to
  // them is written out too.
  if (level >= Level::WARN)
    std::cout.flush();
  notifySinks(level, formatted);
}

void Logger::flush()
{
  std::string pending;
  Level level = Level::INFO;
  {
    LastMessage& last = lastMessage();
    const std::lock_guard lock(last.mutex);
    if (last.repeats > 0)
    {
      pending = formatMessage(last.level,
                              last.message + repeatNote(last.repeats),
                              last.file,
                              last.line,
                              last.func);
      level = last.level;
      last.repeats = 0;
    }
    // Whatever comes next is new, even if it's the same message again.
    last.level = Level::DISABLED;
    last.message.clear();
  }
  if (!pending.empty())
    write(level, pending);
  std::cout.flush();
  std::cerr.flush();
}

void Logger::debug(const char* message, std::size_t value, const char* file)
{
  if (Logger::s_logLevel > Logger::Level::DEBUG)
    return;

  std::fprintf(stdout, "%s[DEBUG] %s: %zu%s\n", BLUE, message, value, RESET);
}

std::string Logger::formatMessage(Logger::Level level,
                                  const std::string& message,
                                  const char* file,
                                  int line,
                                  const char* func)
{
  std::ostringstream ss;

  if (verbosity == Logger::Verbosity::FULL || level >= Logger::Level::WARN)
  {
    ss << currentTime() << " | "
       << "[" << Logger::toString(level) << "] | " << file << " | " << func
       << " | " << line << ": " << message;
  }
  else
  {
    ss << currentTime() << " | " << message;
  }

  return ss.str();
}

std::string Logger::currentTime()
{
  auto now = std::chrono::system_clock::now();
  std::time_t now_c = std::chrono::system_clock::to_time_t(now);

  std::tm local{};
  // localtime_r is POSIX; MSVC provides localtime_s with the arguments swapped.
#if defined(_WIN32)
  localtime_s(&local, &now_c);
#else
  localtime_r(&now_c, &local);
#endif

  std::ostringstream ss;
  ss << std::put_time(&local, "%Y-%m-%d %H:%M:%S");

  return ss.str();
}

void Logger::setLogLevel(Logger::Level logLevel)
{
  Logger::s_logLevel = logLevel;
}

const char* Logger::toString(Logger::Level level)
{
  switch (level)
  {
  case Logger::Level::DEBUG:
    return "DEBUG";
  case Logger::Level::INFO:
    return "INFO";
  case Logger::Level::WARN:
    return "WARN";
  case Logger::Level::ERROR:
    return "ERROR";
  case Logger::Level::DISABLED:
    return "DISABLED";
  }
  return "UNKNOWN";
}

void Logger::setVerbosity(Verbosity v) { Logger::verbosity = v; }

} // namespace sfs
