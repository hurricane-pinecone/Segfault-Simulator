#pragma once

#include "architecture/graph.h"
#include "build/childProcess.h"

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Makes a project's architecture graph on demand: runs `sfs-lsp --graph
// --progress` as a child process, follows the progress it reports on stderr,
// and reads the JSON it writes off the UI thread. Nothing runs between
// start() and poll() but the process and its reader threads.
//
//   generator.start(command);
//
//   // each frame:
//   if (std::optional<architecture::Generated> done = generator.poll())
//     show(*done);
//   else if (generator.running())
//     drawProgress(generator.progress());
namespace architecture
{

/** A finished run: the graph, or why there isn't one. */
struct Generated
{
  /** The graph, if the run worked. */
  std::optional<Graph> graph;
  /** What went wrong, if it didn't. */
  std::string error;
};

class Generator
{
public:
  Generator();

  Generator(const Generator&) = delete;

  /** Stops a run in progress. */
  ~Generator();

  Generator& operator=(const Generator&) = delete;

  /**
   * Check on the run: call each frame while it's running.
   *
   * @return the result, once, when the run has finished and its JSON been
   *         read; nullopt until then
   */
  std::optional<Generated> poll();

  /** How far the run has got, as sfs-lsp last said. */
  Progress progress() const;

  /** Whether a run is under way (sfs-lsp running, or its JSON being read). */
  bool running() const { return m_process || m_parse.valid(); }

  /**
   * Start a run, stopping one in progress.
   *
   * @param command sfs-lsp and its arguments, ready to run
   * @return false (and poll() gives why) if it couldn't be started
   */
  bool start(const std::vector<std::string>& command);

  /** Stop a run in progress; its result never comes. */
  void stop();

private:
  // What the reader threads write and the UI thread reads.
  struct Output
  {
    Output();

    LineSplitter errorLines;
    std::string json;
    std::string messages; // stderr lines that aren't progress
    std::mutex mutex;
    Progress progress;
  };

  std::optional<Generated> m_failed; // a start that failed, for poll()
  std::shared_ptr<Output> m_output;
  std::future<Generated> m_parse;
  std::unique_ptr<ChildProcess> m_process;
  std::string m_program; // for messages
};

} // namespace architecture
