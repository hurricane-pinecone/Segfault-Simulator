#pragma once

#include "build/buildEnvironment.h"
#include "build/childProcess.h"

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

  bool busy() const { return m_process != nullptr; }

  /**
   * Start building `project`'s module.
   *
   * @return false (logged) if a build is already running or can't start.
   */
  bool start(const Project& project);

  /**
   * Advance the build. Returns nothing while it runs (or when idle), then once
   * true (built) or false (failed) when it finishes.
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
};
