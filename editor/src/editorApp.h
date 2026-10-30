#pragma once

#include "build/gameExporter.h"
#include "build/moduleBuilder.h"
#include "editorContext.h"
#include "engine/runtime/scripting/moduleHost.h"
#include "history/editorHistory.h"
#include "panels/panel.h"
#include "scene/sceneDocument.h"
#include "scripting/fieldMigration.h"
#include "scripting/scriptTemplates.h"

#include "engine/runtime/game/game.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
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
class UiPreviewPanel;
class MaterialEditorPanel;

class ViewportPanel;

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
  bool sceneReceivesInput() const override
  {
    return m_context.playing && m_context.gameHasInput;
  }
  bool sceneReceivesMouse() const override { return m_context.gameHasMouse; }
  SceneView sceneView() const override;
  // Game code's quit() stops Play; loadScene() switches scenes within it.
  void onQuit() override;
  void onLoadScene(const std::filesystem::path& scene) override;
  std::filesystem::path gameFolder() const override;
  sfs::StandardScene& replaceScene() override;
  bool ownsWindow() const override { return false; }
  // Files dropped onto the window are imported into the Assets panel's folder.
  void onEvent(const SDL_Event& event) override;

private:
  void drawMenuBar();
  // Along the bottom: what's happening in the background (building scripts,
  // loading models, making previews), with progress bars.
  void drawStatusBar();
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

  // Opening a project: the slow part (scanning its files, which on a WSL
  // share takes a while) runs in the background while the editor carries on;
  // finishOpeningProject() takes over once it's done (a frame's work).
  void openProject(const std::filesystem::path& folder);
  void createProject(const std::filesystem::path& folder,
                     const std::string& name);
  void finishOpeningProject();
  struct OpeningProject
  {
    Project project;
    std::future<sfs::AssetDatabase> assets; // scanned in the background
  };
  std::optional<OpeningProject> m_openingProject;
  // This editor's engine build (see stampModule()), hashed in the background.
  std::shared_future<std::string> m_engineStamp;

  void openScene(const std::filesystem::path& path);
  void saveScene();
  void updateWindowTitle();

  // --- game module & play mode ---
  enum class Mode
  {
    Edit,
    Playing,
  };

  /**
   * Build the module; on success reload it, then play if `thenPlay`. Waits
   * for a UI Preview build, if one's running.
   */
  void requestBuild(bool thenPlay);
  /** The game module is being built (or will be, after a UI Preview build). */
  bool moduleBuilding() const
  {
    return m_moduleBuildQueued ||
           (m_builder.busy() &&
            m_builder.target() == ModuleBuilder::Target::Module);
  }
  /** Swap in the freshly built module, keeping the scene's contents. */
  bool swapModule();

  // Scripts changed (saved in the Code panel, or found changed when the
  // editor regains focus): the game is rebuilt, and saved values follow
  // renamed fields (see swapModule()).
  // UI files (*.ui.h, *.ui.cpp) changed, or the project was opened: the UI
  // Preview's module is rebuilt (in seconds), not the game's. The game's is
  // rebuilt before Play, as always.
  bool m_scriptsChanged = false;
  bool m_uiChanged = false;
  bool m_uiFirstCheck = false; // the next check decides if there's UI to build
  // The next check compares the scripts with the ones the game module was
  // built from (changed while the editor was closed: it's rebuilt).
  bool m_moduleFirstCheck = false;
  std::string m_buildScriptsStamp; // the scripts the running build is of
  bool m_moduleBuildQueued = false; // asked for during a UI Preview build
  struct ScriptsStamp
  {
    std::string scripts; // a hash of scripts/ but the UI files
    std::string ui;      // of the UI files; empty if there are none
  };
  std::future<ScriptsStamp> m_scriptsCheck; // in the background
  std::string m_scriptsStamp;               // the last ones seen
  std::string m_uiStamp;
  bool m_scriptsCheckIsBaseline =
      false;                     // its result is the new normal, not a change
  bool m_scriptsRecheck = false; // check again once it's done
  void checkScripts(bool baseline);
  /** Rewrite the project's scene and prefab files for changed fields. */
  void migrateProjectFiles(const fieldMigration::Changes& changes);
  // A file or folder was moved in the Assets panel: open scenes (and the start
  // scene) follow it.
  void pathMoved(const std::filesystem::path& from,
                 const std::filesystem::path& to);

  // What's open for editing: the scene, and each prefab being edited in its
  // own viewport tab (its own scene, holding the prefab's content alone). One
  // is active, the one last focused: the context points at it and it renders.
  struct Workspace
  {
    sfs::AssetId prefab; // invalid for the scene
    EditorScene* scene = nullptr;
    SceneDocument* document = nullptr;
    EditorHistory* history = nullptr;
    ViewportPanel* viewport = nullptr; // in m_panels
    sfs::Entity selection;             // kept while another is active
    uint64_t appliedRevision = 0;      // the history's, when last applied
    std::unique_ptr<SceneDocument> ownDocument; // a prefab's
    std::unique_ptr<EditorHistory> ownHistory;
  };
  Workspace& sceneWorkspace() { return *m_workspaces.front(); }
  bool editingPrefab() const { return m_active && m_active->prefab.valid(); }
  /** Make `workspace` the one edited and rendered. */
  void activate(Workspace& workspace);
  /** Point the context at the active workspace's scene. */
  void bindContext();
  /** A scene in the scene manager, set up for editing (a prefab's on a grid).
   */
  EditorScene* makeScene(bool forPrefab);

  /** Edit a prefab in its own tab (or bring its tab forward). */
  void openPrefab(const sfs::AssetId& prefab);
  /** Write the prefab from its tab; every copy of it updates. */
  bool applyPrefab(Workspace& workspace);
  /** Whether a prefab's tab differs from its file. */
  bool prefabChanged(Workspace& workspace);
  /** Close a prefab's tab, without applying. */
  void closePrefab(Workspace& workspace);
  /**
   * Rewrite a prefab's file (`write`), then reload the open scenes so their
   * instances of it show the change. `source` (the tab it came from) isn't
   * reloaded; another tab editing the prefab is, unless it has changes.
   */
  bool rewritePrefab(const sfs::AssetId& prefab,
                     const std::function<bool()>& write,
                     const Workspace* source);
  void drawClosePrefabDialog();

  /** Edit a node-graph material in its own tab (or bring its tab forward). */
  void openMaterial(const sfs::AssetId& material);
  /** The material tab whose the Edit menu is (last focused), if any. */
  MaterialEditorPanel* materialPanelWithEdit() const;
  std::vector<MaterialEditorPanel*> m_materialPanels; // in m_panels

  std::vector<std::unique_ptr<Workspace>> m_workspaces; // the scene's first
  Workspace* m_active = nullptr;
  Workspace* m_closing = nullptr;  // a tab asking whether to apply first
  Workspace* m_closeNow = nullptr; // a tab to close next frame (answered)
  std::string m_focusWindow; // brought forward next frame (a reopened tab)
  // Record that the module was just built for this engine (see
  // loadExistingModule()).
  void stampModule();
  /** Load the module from a previous build, if any (on project open). */
  void loadExistingModule();
  void play();
  void stop();
  /**
   * Destroy every scene, freeing everything the loaded module created
   * (component storage, objects, systems) while its code is still loaded.
   */
  void destroyScenes();
  /** Create fresh scenes (after destroyScenes) and point the documents at them.
   */
  void createScenes();
  /** Replace the scene (not prefabs') with a fresh one (Stop, a game's
   * loadScene). */
  void replaceMainScene();

  EditorContext m_context;
  std::optional<SceneDocument> m_document;
  std::optional<sfs::AssetDatabase>
      m_assets; // the open project's assets/ and prefabs/
  AssetBrowserPanel* m_assetPanel = nullptr; // owned by m_panels
  EditorHistory m_history{m_context};
  BuildEnvironment m_environment = BuildEnvironment::current();
  ModuleBuilder m_builder{m_environment};
  sfs::ModuleHost m_module;
  // The project's game module isn't loaded yet (it's never been built, or is
  // being rebuilt): scenes made meanwhile expect its meshes to be missing.
  bool m_awaitingModule = false;
  Mode m_mode = Mode::Edit;
  bool m_playAfterBuild = false;
  nlohmann::json m_editSnapshot; // the scene as it was before Play
  std::vector<std::unique_ptr<Panel>> m_panels;
  CodePanel* m_codePanel = nullptr; // in m_panels
  UiPreviewPanel* m_uiPreview = nullptr;
  std::filesystem::path m_startupProject;
  bool m_layoutChecked = false;
  bool m_appFocused = false; // one of the editor's windows has the keyboard

  // Open/New project dialogs.
  enum class Dialog
  {
    None,
    Open,
    New,
    SaveSceneAs,
    NewScript,
    Export,
    ClosePrefab,
  };
  Dialog m_requestedDialog = Dialog::None;
  std::string m_dialogPath;
  std::string m_dialogName;
  std::string m_dialogSceneName;
  std::string m_dialogScriptName;
  scriptTemplates::Kind m_dialogScriptKind = scriptTemplates::Kind::GameObject;
  std::filesystem::path
      m_dialogScriptFolder; // where it goes (empty: the scripts folder)

  // Export: the dialog's choices, applied once the module has built.
  std::string m_exportFolder;     // the game goes in a subfolder of this
  std::string m_lastExportFolder; // remembered for the next export
  std::string m_exportStartScene; // relative to the project
  int m_exportWindowSize[2] = {1600, 900};
  bool m_exportAfterBuild = false;
};
