#pragma once

#include "build/childProcess.h"
#include "nvim/msgpack.h"
#include "nvim/nvimScreen.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class BuildEnvironment;

// An embedded Neovim (`nvim --embed`) driven through its msgpack-RPC UI
// protocol: the editor attaches as a UI, receives screen updates into an
// NvimScreen, and sends input. Neovim runs on the build machine (in WSL for a
// Windows editor), so file paths go through the BuildEnvironment.
class NvimClient
{
public:
  // Which configuration Neovim starts with.
  enum class ConfigMode
  {
    Auto,   // the user's own config if they have one, otherwise the engine's
    Engine, // always the engine's lightweight config (isolated from the user's)
    User,   // always the user's config
  };

  struct LaunchOptions
  {
    ConfigMode mode = ConfigMode::Auto;
    std::filesystem::path engineConfig; // folder holding the engine's init.lua
    std::filesystem::path workingDirectory;
    bool icons = false; // the UI's font draws Nerd Font icons (g:sfs_icons)
    /**
     * The tools the editor ships (nvim/, ripgrep/, clangd/), used when Neovim
     * runs on the editor's own machine. Without them Neovim and its tools come
     * from PATH, as they always do in WSL.
     */
    std::filesystem::path tools;
  };

  /** How start() runs Neovim. */
  struct LaunchCommand
  {
    /** The command line, wrapped to run on the build machine. */
    std::vector<std::string> args;
    /** Whether it runs the shipped Neovim (LaunchOptions::tools). */
    bool bundled = false;
    /**
     * Variables set in Neovim's environment over the editor's own. Run in WSL,
     * they are on the command line instead (`env NAME=value nvim`).
     */
    ChildProcess::Environment environment;
  };

  explicit NvimClient(const BuildEnvironment& environment);
  ~NvimClient();

  /** The project script being edited last (an editor path), if any. */
  const std::filesystem::path& focusedScript() const { return m_focusedScript; }

  /**
   * How many times a project script has been entered (its buffer, the same
   * one again included): it goes up as focusedScript() is told again.
   */
  uint64_t focusedScriptRevision() const { return m_focusedRevision; }

  /** Keys in Neovim's notation, e.g. "i", "<C-s>", "<lt>". */
  void input(const std::string& keys);

  /**
   * The command start() runs. On the editor's machine it is the shipped
   * Neovim, when LaunchOptions::tools has it, with the shipped tools first on
   * PATH; otherwise (or in WSL) `nvim` from PATH.
   *
   * @param options how Neovim is to start
   * @return the command line, and the environment it runs with
   */
  LaunchCommand launchCommand(const LaunchOptions& options) const;

  /** A mouse event, as nvim_input_mouse takes it. */
  void mouse(const char* button,
             const char* action,
             const std::string& modifiers,
             int row,
             int column);

  /**
   * Open a file in the current window.
   *
   * @param file the file, as the editor's path
   * @param line the line to put the cursor on (from 1); 0 leaves it where
   *        Neovim puts it
   */
  void open(const std::filesystem::path& file, int line = 0);

  /** The project scripts open in Neovim (listed buffers), as editor paths. */
  const std::vector<std::filesystem::path>& openScripts() const
  {
    return m_openScripts;
  }

  /** Apply everything Neovim sent since the last call (main thread). */
  void poll();

  /** Resize the UI grid (only sent when it changes). */
  void resize(int columns, int rows);

  /** Whether Neovim is running (false once it quits, e.g. after :qa). */
  bool running();

  const NvimScreen& screen() const { return m_screen; }

  /** Point Neovim at another project (its working directory). */
  void setProject(const std::filesystem::path& root);

  /** Start Neovim and attach as a `columns` x `rows` UI. */
  bool start(const LaunchOptions& options, int columns, int rows);

  /** Quit Neovim, saving modified files (killed if it doesn't exit in time). */
  void stop();

  /** True once after a file in the project's scripts/ was saved (the config
   * reports it). */
  bool takeScriptSaved() { return std::exchange(m_scriptSaved, false); }

private:
  void handle(const msgpack::Value& message);
  void receive(std::string_view chunk); // reader thread
  // Fire-and-forget: Neovim's replies are only checked for errors.
  void request(const std::string& method, const msgpack::Array& params);

  const BuildEnvironment& m_environment;
  std::unique_ptr<ChildProcess> m_process;
  NvimScreen m_screen;
  bool m_scriptSaved = false;
  std::filesystem::path m_focusedScript;
  uint64_t m_focusedRevision = 0;
  std::vector<std::filesystem::path> m_openScripts;
  int m_columns = 0;
  int m_rows = 0;
  uint32_t m_nextRequestId = 1;

  // Bytes from the reader thread, decoded into whole messages for poll().
  std::mutex m_incomingMutex;
  std::string m_incomingBytes;
  std::deque<msgpack::Value> m_incoming;
};
