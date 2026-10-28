#pragma once

#include "build/childProcess.h"
#include "nvim/msgpack.h"
#include "nvim/nvimScreen.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
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
  };

  explicit NvimClient(const BuildEnvironment& environment);
  ~NvimClient();

  /** Start Neovim and attach as a `columns` x `rows` UI. */
  bool start(const LaunchOptions& options, int columns, int rows);

  /** Quit Neovim, saving modified files (killed if it doesn't exit in time). */
  void stop();

  /** Whether Neovim is running (false once it quits, e.g. after :qa). */
  bool running();

  /** Apply everything Neovim sent since the last call (main thread). */
  void poll();

  const NvimScreen& screen() const { return m_screen; }

  /** Resize the UI grid (only sent when it changes). */
  void resize(int columns, int rows);

  /** Keys in Neovim's notation, e.g. "i", "<C-s>", "<lt>". */
  void input(const std::string& keys);

  /** A mouse event, as nvim_input_mouse takes it. */
  void mouse(const char* button, const char* action, const std::string& modifiers, int row, int column);

  /** Point Neovim at another project (its working directory). */
  void setProject(const std::filesystem::path& root);

  /** Open a file (an editor path) in the current window. */
  void open(const std::filesystem::path& file);

  /** True once after a file in the project's scripts/ was saved (the config reports it). */
  bool takeScriptSaved() { return std::exchange(m_scriptSaved, false); }

private:
  // Fire-and-forget: Neovim's replies are only checked for errors.
  void request(const std::string& method, const msgpack::Array& params);
  void receive(std::string_view chunk); // reader thread
  void handle(const msgpack::Value& message);

  const BuildEnvironment& m_environment;
  std::unique_ptr<ChildProcess> m_process;
  NvimScreen m_screen;
  bool m_scriptSaved = false;
  int m_columns = 0;
  int m_rows = 0;
  uint32_t m_nextRequestId = 1;

  // Bytes from the reader thread, decoded into whole messages for poll().
  std::mutex m_incomingMutex;
  std::string m_incomingBytes;
  std::deque<msgpack::Value> m_incoming;
};
