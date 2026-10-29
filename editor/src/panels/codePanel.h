#pragma once

#include "nvim/nvimClient.h"
#include "panels/panel.h"

#include "imgui.h"

#include <filesystem>
#include <optional>

class BuildEnvironment;

// The script editor: an embedded Neovim drawn inside a dockable panel. Neovim
// starts when the panel is first shown (or a script is opened) with the
// engine's lightweight config, or the user's own; see NvimClient::ConfigMode.
// While the panel has focus it receives the keyboard, editor shortcuts
// included (Ctrl+S saves the file, not the scene).
class CodePanel : public Panel
{
public:
  // Per-user, saved in imgui.ini.
  struct Settings
  {
    NvimClient::ConfigMode mode = NvimClient::ConfigMode::Auto;
    float fontSize = 15.0f;
  };

  explicit CodePanel(const BuildEnvironment& environment);
  ~CodePanel() override;

  /** Open a file in Neovim, showing and focusing the panel. */
  void openFile(const std::filesystem::path& file);

  /** True once after a script was saved in the panel (so the game can be rebuilt). */
  bool takeScriptSaved() { return m_nvim.takeScriptSaved(); }

  /** The project script being edited last (empty: none yet). */
  const std::filesystem::path& focusedScript() const { return m_nvim.focusedScript(); }

  void update(EditorContext& context) override;

protected:
  void drawContents(EditorContext& context) override;
  void beforeBegin() override;
  int windowFlags() const override;

private:
  bool ensureStarted(const EditorContext& context);
  void drawToolbar(EditorContext& context);
  void drawScreen(const ImVec2& origin, float cellWidth, float cellHeight);
  void handleKeyboard();
  void handleMouse(const ImVec2& origin, float cellWidth, float cellHeight);

  const BuildEnvironment& m_environment;
  NvimClient m_nvim;
  Settings m_settings;
  bool m_started = false;      // launched at least once (so an exit is shown)
  bool m_failed = false;       // couldn't launch: don't retry every frame
  bool m_focusRequested = false;
  bool m_dockChecked = false;
  int m_columns = 80;
  int m_rows = 24;
  std::filesystem::path m_projectRoot;                // Neovim's working directory
  std::optional<std::filesystem::path> m_pendingFile; // opened once Neovim runs
  int m_mouseButtonDown = -1;                         // ImGuiMouseButton being dragged
  int m_mouseRow = 0;
  int m_mouseColumn = 0;
  ImFont* m_font = nullptr; // a monospace font with wide coverage, or the UI font
};
