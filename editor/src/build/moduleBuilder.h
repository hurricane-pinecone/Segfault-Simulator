#pragma once

#include "build/buildEnvironment.h"
#include "build/childProcess.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Project;

// Builds a project's game module in the background: configures the project's
// CMake build the first time, then builds it, streaming the output into the
// log (so the console shows it, errors highlighted). Poll it every frame.
class ModuleBuilder
{
public:
  explicit ModuleBuilder(BuildEnvironment environment)
      : m_environment(std::move(environment))
  {
  }

  ~ModuleBuilder() { cancel(); }

  ModuleBuilder(const ModuleBuilder&) = delete;
  ModuleBuilder& operator=(const ModuleBuilder&) = delete;

  bool busy() const { return m_process != nullptr; }

  /** How far the running build is, 0-100, as the build tool reports it; -1 if it hasn't said. */
  int percent() const { return m_percent; }

  /**
   * Start building `project`'s module.
   *
   * @return false (logged) if a build is already running or can't start.
   */
  bool start(const Project& project);

  /**
   * Stop the running build, compilers and all. The next poll() reports it
   * failed.
   */
  void cancel();

  /**
   * Advance the build. Returns nothing while it runs (or when idle), then once
   * true (built) or false (failed, or cancelled) when it finishes.
   */
  std::optional<bool> poll();

  /** Where `project`'s built module lands, as the editor sees it. */
  std::filesystem::path modulePath(const Project& project) const;

  const BuildEnvironment& environment() const { return m_environment; }

private:
  bool startNextStep();
  std::filesystem::path buildDirectory(const Project& project) const;

  BuildEnvironment m_environment;
  std::vector<std::vector<std::string>> m_steps; // commands still to run
  std::unique_ptr<ChildProcess> m_process;
  std::atomic<int> m_percent = -1; // written by the output reader thread
  std::atomic<int> m_group = -1;   // the build's process group on the build machine (see stoppableCommand)
  bool m_cancelled = false;        // for poll() to report
};
