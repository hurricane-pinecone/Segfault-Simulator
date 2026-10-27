#pragma once

#include "editorContext.h"
#include "panels/panel.h"

#include "engine/runtime/game/game.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The SfsEditor application: a Game on the WebGPU backend whose frame is the
// editor UI. A dockspace fills the window; panels (viewport, hierarchy,
// inspector, console, scene settings) dock into it or drag out into their own
// OS windows.
class EditorApp : public sfs::Game
{
public:
  /** Open `projectFolder` once the editor is set up (empty: none). */
  explicit EditorApp(std::filesystem::path projectFolder = {});

protected:
  std::unique_ptr<sfs::IRenderBackend> makeRenderBackend() override;
  void onSetup() override;
  void onUpdate(double deltaTime) override;
  void onRender() override;
  void onDestroy() override;

private:
  void drawMenuBar();
  void drawProjectDialogs();
  void buildDefaultLayout(unsigned int dockspaceId);

  void openProject(const std::filesystem::path& folder);
  void createProject(const std::filesystem::path& folder, const std::string& name);
  void updateWindowTitle();

  EditorContext m_context;
  std::vector<std::unique_ptr<Panel>> m_panels;
  std::filesystem::path m_startupProject;
  bool m_layoutChecked = false;

  // Open/New project dialogs.
  enum class Dialog
  {
    None,
    Open,
    New,
  };
  Dialog m_requestedDialog = Dialog::None;
  std::string m_dialogPath;
  std::string m_dialogName;
};
