#pragma once

#include <cstring>
#include <functional>
#include <string>

namespace sfs
{

/**
 * The engine's log: messages go to stdout/stderr and to any sinks (an editor's
 * console, a game's log file).
 *
 * Debug builds log everything from DEBUG up; release builds (NDEBUG) from INFO
 * up. Output is buffered, and flushed on a warning or an error (and by
 * flush()), so a chatty game doesn't stall on its log. A message repeated
 * straight after itself within a second is counted rather than written again:
 * the count follows ("... (repeated 41 more times)") once something else is
 * logged, the second is up, or on flush().
 */
class Logger
{
public:
  enum class Level
  {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    DISABLED
  };

  enum class Verbosity
  {
    FULL,   // file, line, function
    MINIMAL // just timestamp + message
  };

  static void setLogLevel(Level level);
  static Level logLevel() { return s_logLevel; }
  static void setVerbosity(Verbosity v);

  /**
   * Write out anything held back: a pending repeat count, and buffered
   * stdout/stderr. Sinks are told of the repeat count like any message.
   */
  static void flush();

  /** Receives every message that passes the log level, already formatted. */
  using Sink = std::function<void(Level level, const std::string& message)>;
  using SinkId = int;

  /**
   * Also send messages to `sink` (e.g. an editor's console panel), alongside
   * stdout/stderr. Thread-safe. A sink is called without any lock held, so it
   * may add or remove sinks; what it logs itself reaches only stdout/stderr.
   * removeSink() waits for the sink's calls on other threads to finish.
   *
   * @return an id for removeSink.
   */
  static SinkId addSink(Sink sink);
  static void removeSink(SinkId id);

  static void info(const std::string& message,
                   const char* file,
                   int line,
                   const char* func);
  static void debug(const std::string& message,
                    const char* file,
                    int line,
                    const char* func);

  static void debug(const char* message, std::size_t value, const char* file);

  static void warn(const std::string& message,
                   const char* file,
                   int line,
                   const char* func);

  static void error(const std::string& message,
                    const char* file,
                    int line,
                    const char* func);

private:
  static Level s_logLevel;
  static Verbosity verbosity;

  inline static constexpr const char* RED = "\033[31m";
  inline static constexpr const char* BLUE = "\033[94m";
  inline static constexpr const char* YELLOW = "\033[33m";
  inline static constexpr const char* RESET = "\033[0m";

  static std::string formatMessage(Level level,
                                   const std::string& message,
                                   const char* file,
                                   int line,
                                   const char* func);
  static std::string currentTime();

  // Forward a formatted message to every registered sink.
  static void notifySinks(Level level, const std::string& message);

  // Log a message that passed the level: counted if it repeats the last one,
  // else written (after the last one's repeat count, if any).
  static void log(Level level,
                  const std::string& message,
                  const char* file,
                  int line,
                  const char* func);
  // Write a formatted message to stdout/stderr and the sinks.
  static void write(Level level, const std::string& formatted);

  static const char* toString(Level level);
};

inline const char* __log_file_name(const char* path)
{
  const char* slash = std::strrchr(path, '/');
  const char* backslash = std::strrchr(path, '\\');
  const char* sep = slash > backslash ? slash : backslash;
  return sep ? sep + 1 : path;
}

} // namespace sfs

// Compiler-decorated function name for log call sites. GCC/Clang spell it
// __PRETTY_FUNCTION__; MSVC spells it __FUNCSIG__.
#if defined(_MSC_VER)
  #define SFS_PRETTY_FUNCTION __FUNCSIG__
#else
  #define SFS_PRETTY_FUNCTION __PRETTY_FUNCTION__
#endif

#define LOG_INFO(message)                                                      \
  sfs::Logger::info(                                                           \
      message, sfs::__log_file_name(__FILE__), __LINE__, SFS_PRETTY_FUNCTION)

#define LOG_DEBUG(message)                                                     \
  sfs::Logger::debug(                                                          \
      message, sfs::__log_file_name(__FILE__), __LINE__, SFS_PRETTY_FUNCTION)

#define LOG_DEBUG_SIZE(label, value)                                           \
  sfs::Logger::debug(label, value, sfs::__log_file_name(__FILE__))

#define LOG_WARN(message)                                                      \
  sfs::Logger::warn(                                                           \
      message, sfs::__log_file_name(__FILE__), __LINE__, SFS_PRETTY_FUNCTION)

#define LOG_ERROR(message)                                                     \
  sfs::Logger::error(                                                          \
      message, sfs::__log_file_name(__FILE__), __LINE__, SFS_PRETTY_FUNCTION)
