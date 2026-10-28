#pragma once

#include "build/gameExporter.h"
#include "build/moduleBuilder.h"
#include "editorContext.h"
#include "history/editorHistory.h"
#include "panels/panel.h"
#include "scene/sceneDocument.h"
#include "engine/runtime/scripting/moduleHost.h"
#include "scripting/scriptTemplates.h"

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
//
// Play builds the project's scripts into its game module, reloads it, and runs
// the scene's scripts; Stop restores the scene as it was before Play.
class AssetBrowserPanel;
class CodePanel;

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
  bool sceneReceivesInput() const override { return m_context.playing && m_context.gameHasInput; }
  SceneView sceneView() const override;
  // Game code's quit() stops Play; loadScene() switches scenes within it.
  void onQuit() override;
  void onLoadScene(const std::filesystem::path& scene) override;
  std::filesystem::path gameFolder() const override;
  sfs::Scene3D& replaceScene() override;
  bool ownsWindow() const override { return false; }
  // Files dropped onto the window are imported into the Assets panel's folder.
  void onEvent(const SDL_Event& event) override;

private:
  void drawMenuBar();
  void drawProjectDialogs();
  void buildDefaultLayout(unsigned int dockspaceId);

  void drawSceneMenu();
  void drawEditMenu();
  void handleShortcuts();
  void duplicateSelection();
  void deleteSelection();
  // Move the selection to the viewport's view (e.g. to aim a camera), or the
  // view to the selection (e.g. to see what a camera sees).
  void alignSelectionWithView();
  void alignViewWithSelection();
  void drawScriptsMenu();
  void drawPlayControls();
  void drawExportDialog();
  /** Export the game once its module is built (after requestBuild). */
  void exportGame();
  /** Where the game goes: its own folder inside the chosen one. */
  std::filesystem::path exportTarget() const;

  void openProject(const std::filesystem::path& folder);
  void createProject(const std::filesystem::path& folder, const std::string& name);
  void projectOpened();

  void openScene(const std::filesystem::path& path);
  void saveScene();
  void updateWindowTitle();

  // --- game module & play mode ---
  enum class Mode
  {
    Edit,
    Playing,
  };

  /** Build the module; on success reload it, then play if `thenPlay`. */
  void requestBuild(bool thenPlay);
  /** Swap in the freshly built module, keeping the scene's contents. */
  bool swapModule();
  /** Load the module from a previous build, if any (on project open). */
  void loadExistingModule();
  void play();
  void stop();
  /**
   * Destroy the scene, freeing everything the loaded module created
   * (component storage, objects, systems) while its code is still loaded.
   */
  void destroyScene();
  /** Create a fresh scene (after destroyScene) and point the document at it. */
  void createScene();

  EditorContext m_context;
  std::optional<SceneDocument> m_document;
  std::optional<sfs::AssetDatabase> m_assets; // the open project's assets/
  AssetBrowserPanel* m_assetPanel = nullptr;  // owned by m_panels
  EditorHistory m_history{m_context};
  BuildEnvironment m_environment = BuildEnvironment::current();
  ModuleBuilder m_builder{m_environment};
  sfs::ModuleHost m_module;
  Mode m_mode = Mode::Edit;
  bool m_playAfterBuild = false;
  nlohmann::json m_editSnapshot; // the scene as it was before Play
  std::vector<std::unique_ptr<Panel>> m_panels;
  CodePanel* m_codePanel = nullptr; // in m_panels
  std::filesystem::path m_startupProject;
  bool m_layoutChecked = false;

  // Open/New project dialogs.
  enum class Dialog
  {
    None,
    Open,
    New,
    SaveSceneAs,
    NewScript,
    Export,
  };
  Dialog m_requestedDialog = Dialog::None;
  std::string m_dialogPath;
  std::string m_dialogName;
  std::string m_dialogSceneName;
  std::string m_dialogScriptName;
  scriptTemplates::Kind m_dialogScriptKind = scriptTemplates::Kind::GameObject;

  // Export: the dialog's choices, applied once the module has built.
  std::string m_exportFolder;     // the game goes in a subfolder of this
  std::string m_lastExportFolder; // remembered for the next export
  std::string m_exportStartScene; // relative to the project
  int m_exportWindowSize[2] = {1600, 900};
  bool m_exportAfterBuild = false;
};
