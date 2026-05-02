#pragma once

#include <string>

namespace sfs
{

class Logger
{
public:
  enum class Level
  {
    DEBUG,
    INFO,
    ERROR,
    DISABLED
  };

  enum class Verbosity
  {
    FULL,   // file, line, function
    MINIMAL // just timestamp + message
  };

  static void setLogLevel(Level level);
  static void setVerbosity(Verbosity v);

  static void info(const std::string& message,
                   const char* file,
                   int line,
                   const char* func);
  static void debug(const std::string& message,
                    const char* file,
                    int line,
                    const char* func);

  static void debug(const char* message, std::size_t value, const char* file);

  static void error(const std::string& message,
                    const char* file,
                    int line,
                    const char* func);

private:
  static Level logLevel;
  static Verbosity verbosity;

  inline static constexpr const char* RED = "\033[31m";
  inline static constexpr const char* BLUE = "\033[94m";
  inline static constexpr const char* RESET = "\033[0m";

  static std::string formatMessage(Level level,
                                   const std::string& message,
                                   const char* file,
                                   int line,
                                   const char* func);
  static std::string currentTime();

  static const char* toString(Level level);
};

inline const char* __log_file_name(const char* path)
{
  const char* file = std::strrchr(path, '/');
  return file ? file + 1 : path;
}

} // namespace sfs

#define LOG_INFO(message)                                                      \
  Logger::info(                                                                \
      message, __log_file_name(__FILE__), __LINE__, __PRETTY_FUNCTION__)

#define LOG_DEBUG(message)                                                     \
  Logger::debug(                                                               \
      message, __log_file_name(__FILE__), __LINE__, __PRETTY_FUNCTION__)

#define LOG_DEBUG_SIZE(label, value)                                           \
  Logger::debug(label, value, __log_file_name(__FILE__))

#define LOG_ERROR(message)                                                     \
  Logger::error(                                                               \
      message, __log_file_name(__FILE__), __LINE__, __PRETTY_FUNCTION__)
