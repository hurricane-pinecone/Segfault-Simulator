#include "logger.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger::Level Logger::logLevel = Logger::Level::DEBUG;
Logger::Verbosity Logger::verbosity = Logger::Verbosity::MINIMAL;

void Logger::info(const std::string& message,
                  const char* file,
                  int line,
                  const char* func)
{
  if (Logger::logLevel > Logger::Level::INFO)
    return;

  std::cout << formatMessage(Logger::Level::INFO, message, file, line, func)
            << std::endl;
}

void Logger::error(const std::string& message,
                   const char* file,
                   int line,
                   const char* func)
{
  if (Logger::logLevel > Logger::Level::ERROR)
    return;

  std::cerr << RED
            << formatMessage(Logger::Level::ERROR, message, file, line, func)
            << RESET << std::endl;
}

void Logger::debug(const std::string& message,
                   const char* file,
                   int line,
                   const char* func)
{
  if (Logger::logLevel > Logger::Level::DEBUG)
    return;

  std::cout << BLUE
            << formatMessage(Logger::Level::DEBUG, message, file, line, func)
            << RESET << std::endl;
}

std::string Logger::formatMessage(Logger::Level level,
                                  const std::string& message,
                                  const char* file,
                                  int line,
                                  const char* func)
{
  std::ostringstream ss;

  if (verbosity == Logger::Verbosity::FULL || level == Logger::Level::ERROR)
  {
    ss << currentTime() << " | " << "[" << Logger::toString(level) << "] | "
       << file << " | " << func << " | " << line << ": " << message;
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
  localtime_r(&now_c, &local);

  std::ostringstream ss;
  ss << std::put_time(&local, "%Y-%m-%d %H:%M:%S");

  return ss.str();
}

void Logger::setLogLevel(Logger::Level logLevel)
{
  Logger::logLevel = logLevel;
}

const char* Logger::toString(Logger::Level level)
{
  switch (level)
  {
  case Logger::Level::DEBUG:
    return "DEBUG";
  case Logger::Level::INFO:
    return "INFO";
  case Logger::Level::ERROR:
    return "ERROR";
  case Logger::Level::DISABLED:
    return "DISABLED";
  }
  return "UNKNOWN";
}

void Logger::setVerbosity(Verbosity v) { Logger::verbosity = v; }
