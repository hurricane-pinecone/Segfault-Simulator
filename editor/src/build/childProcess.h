#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// A process the editor runs without blocking. Its output arrives on reader
// threads in raw chunks (binary-safe, for protocols such as Neovim's msgpack
// RPC); its stdin can optionally be written. The UI keeps polling running().
//
// It runs with everything it starts as one group (a process group; a job
// object on Windows), so stopping it stops them too, and on Windows they all
// go when the editor does.
class ChildProcess
{
public:
  using OutputHandler = std::function<void(std::string_view chunk)>;

  struct Options
  {
    // Receives stderr separately; when empty, stderr is merged into stdout.
    OutputHandler onStderr;
    // Give the process a stdin pipe that write() feeds.
    bool pipeStdin = false;
  };

  ChildProcess();
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /**
   * Start `args[0]` with the remaining arguments (searched on PATH). Handlers
   * are called from reader threads.
   *
   * @return false (logged) if it couldn't be started.
   */
  bool start(const std::vector<std::string>& args, OutputHandler onStdout, Options options);
  bool start(const std::vector<std::string>& args, OutputHandler onStdout);

  /** Send bytes to the process's stdin (needs Options::pipeStdin). Thread-safe. */
  bool write(std::string_view data);

  /** Whether it is still running (non-blocking). */
  bool running();

  /** Stop it and everything it started, and wait for it to go. */
  void terminate();

  /** Exit code once finished; -1 if it couldn't start or was killed. */
  int exitCode() const { return m_exitCode; }

private:
  struct Platform; // OS process and pipe handles
  void finish();

  std::unique_ptr<Platform> m_platform;
  std::vector<std::thread> m_readers;
  std::mutex m_writeMutex;
  int m_exitCode = -1;
  bool m_started = false;
};

// Turns output chunks into lines (dropping \r). Not thread-safe: feed it from
// one reader thread.
class LineSplitter
{
public:
  using LineHandler = std::function<void(const std::string& line)>;

  explicit LineSplitter(LineHandler onLine) : m_onLine(std::move(onLine)) {}

  void feed(std::string_view chunk);

  /** Emit a final line that had no trailing newline. */
  void flush();

private:
  LineHandler m_onLine;
  std::string m_pending;
};
