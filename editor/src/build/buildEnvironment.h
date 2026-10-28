#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// How game modules are built: which toolchain, against which engine package,
// with which code generator -- baked in when the editor itself is built, so a
// module always matches the engine the editor runs.
//
// The editor may run on Windows while the toolchain lives in WSL (the engine's
// cross-build setup). Then build commands run through `wsl.exe`, and paths are
// translated between the editor's view (\\wsl.localhost\<distro>\...) and the
// build machine's (/home/...).
class BuildEnvironment
{
public:
  enum class Runner
  {
    Native, // the editor and the toolchain share a machine
    Wsl,    // Windows editor, WSL toolchain
  };

  /** The configuration this editor was built with. */
  static BuildEnvironment current();

  Runner runner() const { return m_runner; }
  const std::string& toolchainFile() const { return m_toolchainFile; }
  const std::string& enginePrefix() const { return m_enginePrefix; }
  const std::string& codegen() const { return m_codegen; }
  const std::string& generator() const { return m_generator; }
  const std::string& buildType() const { return m_buildType; }

  /** File extension of a built module (".dll", ".so"). */
  const std::string& moduleSuffix() const { return m_moduleSuffix; }

  /** The target platform's folder name for build output ("windows", "linux"). */
  std::string platformName() const { return m_moduleSuffix == ".dll" ? "windows" : "linux"; }

  /** An editor path as the build machine sees it. */
  std::string toBuildPath(const std::filesystem::path& editorPath) const;

  /** A build-machine path as the editor sees it. */
  std::filesystem::path toEditorPath(const std::string& buildPath) const;

  /** `args` wrapped to run on the build machine. */
  std::vector<std::string> command(std::vector<std::string> args) const;

  /**
   * `args` wrapped to run on the build machine so it can be stopped, with
   * everything it starts. Run through WSL, it runs in a Linux process group
   * of its own and first prints "<kGroupMarker> <group id>" for
   * stopCommand(); stopping the local process alone would leave it running.
   */
  std::vector<std::string> stoppableCommand(std::vector<std::string> args) const;
  static constexpr std::string_view kGroupMarker = "@sfs-group";
  /** What stops a stoppableCommand() that printed `group` (nothing to run: stop the process itself). */
  std::vector<std::string> stopCommand(int group) const;

private:
  Runner m_runner = Runner::Native;
  std::string m_wslDistro;
  std::string m_toolchainFile;
  std::string m_enginePrefix;
  std::string m_codegen;
  std::string m_generator;
  std::string m_buildType;
  std::string m_moduleSuffix;
};
