#include "editorApp.h"

#include "panels/assetBrowserPanel.h"
#include "panels/codePanel.h"
#include "panels/uiPreviewPanel.h"
#include "panels/consolePanel.h"
#include "panels/hierarchyPanel.h"
#include "panels/inspectorPanel.h"
#include "panels/materialEditorPanel.h"
#include "panels/sceneSettingsPanel.h"
#include "panels/viewportPanel.h"
#include "platform/fileDialog.h"
#include "platform/paths.h"
#include "platform/shell.h"
#include "scene/editorScene.h"
#include "scene/gridModule.h"
#include "widgets/widgets.h"

#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/serialization/prefabComponents.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "engine/webgpu/webGpuRenderBackend.h"
#include "scene/prefabs.h"
#include "widgets/theme.h"

#include "engine/core/logger/logger.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder (default layout)
#include "imguizmo/ImGuizmo.h"

#include <SDL_filesystem.h>
#include <SDL_keyboard.h>
#include <SDL_video.h>

#include <algorithm>
#include <fstream>
#include <utility>

namespace
{
constexpr const char* kOpenDialog = "Open Project";
constexpr const char* kNewDialog = "New Project";
constexpr const char* kSaveSceneDialog = "Save Scene As";
constexpr const char* kNewScriptDialog = "New Script";
constexpr const char* kExportDialog = "Export Game";
constexpr const char* kClosePrefabDialog = "Close Prefab";

// The project's scene files, sorted by name.
std::vector<std::filesystem::path> sceneFiles(const Project& project)
{
  std::vector<std::filesystem::path> files;
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(project.scenesFolder(), error))
    if (entry.is_regular_file() &&
        entry.path().extension() == SceneDocument::kExtension)
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  return files;
}

// Which build of the engine this editor runs (defined below).
std::string engineBuildStamp();

// Beside a module: the engineBuildStamp() it was built for.
std::filesystem::path stampPath(const std::filesystem::path& module)
{
  return module.string() + ".engine";
}

// Beside a module: the hash of the scripts it was built from.
std::filesystem::path scriptsStampPath(const std::filesystem::path& module)
{
  return module.string() + ".scripts";
}
} // namespace

EditorApp::EditorApp(std::filesystem::path projectFolder)
    : m_startupProject(std::move(projectFolder))
{
}

std::unique_ptr<sfs::IRenderBackend> EditorApp::makeRenderBackend()
{
  // An editor needn't redraw faster than the display: vsync keeps it from
  // running the GPU flat out.
  auto backend = std::make_unique<sfs::WebGpuRenderBackend>();
  backend->context().setVsync(true);
  return backend;
}

void EditorApp::onSetup()
{
  // The editor draws its own UI; the engine's debug overlay would duplicate it.
  setDebugUiVisible(false);
  m_context.history = &m_history;

  auto& backend = static_cast<sfs::WebGpuRenderBackend&>(*renderBackend());

  m_panels.push_back(
      std::make_unique<ConsolePanel>()); // first, to catch setup logs
  // The editor's look; the UI font first, before the Code panel adds its own.
  theme::loadUiFont();
  theme::apply();
  auto sceneViewport = std::make_unique<ViewportPanel>(backend);
  auto& scene = *m_workspaces.emplace_back(std::make_unique<Workspace>());
  scene.history = &m_history;
  scene.viewport = sceneViewport.get();
  m_active = &scene;
  m_panels.push_back(std::move(sceneViewport));
  m_panels.push_back(std::make_unique<HierarchyPanel>());
  m_panels.push_back(std::make_unique<InspectorPanel>());
  m_panels.push_back(std::make_unique<SceneSettingsPanel>());
  auto codePanel = std::make_unique<CodePanel>(m_environment);
  m_codePanel = codePanel.get();
  m_panels.push_back(std::move(codePanel));
  auto uiPreview = std::make_unique<UiPreviewPanel>(
      backend.context(), *backend.sceneServices(), *m_codePanel);
  m_uiPreview = uiPreview.get();
  auto assetPanel = std::make_unique<AssetBrowserPanel>(
      AssetBrowserPanel::Actions{
          .openScene = [this](const std::filesystem::path& path)
          { openScene(path); },
          .openText = [this](const std::filesystem::path& path)
          { m_codePanel->openFile(path); },
          .openPrefab =
              [this](const std::filesystem::path& path)
          {
            if (m_assets)
              if (const sfs::AssetRecord* record = m_assets->findByPath(
                      path.lexically_relative(m_assets->root())))
                m_context.openPrefab = record->id;
          },
          .newScript =
              [this](scriptTemplates::Kind kind,
                     const std::filesystem::path& folder)
          {
            m_dialogScriptKind = kind;
            m_dialogScriptFolder = folder;
            m_dialogScriptName.clear();
            m_requestedDialog = Dialog::NewScript;
          },
          .moved = [this](const std::filesystem::path& from,
                          const std::filesystem::path& to)
          { pathMoved(from, to); }},
      backend.context());
  m_assetPanel = assetPanel.get();
  m_panels.push_back(std::move(assetPanel));
  // After the Console and Assets, to join their dock node.
  m_panels.push_back(std::move(uiPreview));

  // The Assets panel draws prefab previews from the scene's meshes, so keep
  // their vertices (before any scene makes meshes).
  sfs::MeshLibrary::setKeepVertices(true);

  m_context.rewritePrefab =
      [this](const sfs::AssetId& prefab, const std::function<bool()>& write)
  { return rewritePrefab(prefab, write, nullptr); };

  // Hashing the engine's libraries reads tens of megabytes: start now.
  m_engineStamp =
      std::async(std::launch::async, [] { return engineBuildStamp(); }).share();

  createScenes();
  // Scenes open at once, their models appearing as they load.
  m_context.scene->assetStore().setBackgroundLoading(true);

  // A project to open: start empty rather than showing the starter scene
  // while it opens.
  if (!m_startupProject.empty())
  {
    m_context.scene->clear();
    openProject(m_startupProject);
  }
  updateWindowTitle();
}

void EditorApp::onDestroy()
{
  // Save the layout (with the panels' own settings, which only they can
  // write) now, before the panels go; ImGui's own save at shutdown would
  // leave them out.
  if (ImGui::GetCurrentContext() && ImGui::GetIO().IniFilename)
  {
    ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
    ImGui::GetIO().IniFilename = nullptr;
  }
  // Panels hold the backend's scene target and a logger sink; release them
  // while both still exist.
  m_materialPanels.clear();
  m_panels.clear();
}

void EditorApp::onUpdate(double)
{
  // A project being opened: carry on once its files are scanned.
  if (m_openingProject &&
      m_openingProject->assets.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready)
    finishOpeningProject();

  // Scripts saved, or changed while the editor was away: rebuild the game
  // (once nothing else is building, and not mid-Play).
  // A save checks the scripts against the last build's, so saving several
  // files at once (or saving unchanged ones) builds once, not per save.
  if (m_codePanel && m_codePanel->takeScriptSaved())
    checkScripts(false);
  if (m_scriptsCheck.valid() && m_scriptsCheck.wait_for(std::chrono::seconds(
                                    0)) == std::future_status::ready)
  {
    const ScriptsStamp stamp = m_scriptsCheck.get();
    if (!m_scriptsCheckIsBaseline && !m_scriptsStamp.empty() &&
        stamp.scripts != m_scriptsStamp)
      m_scriptsChanged = true;
    if (m_scriptsCheckIsBaseline)
      m_buildScriptsStamp = stamp.scripts;
    if (std::exchange(m_moduleFirstCheck, false) && m_context.project)
    {
      const std::filesystem::path module =
          m_builder.modulePath(*m_context.project);
      std::string builtFrom;
      std::ifstream(scriptsStampPath(module)) >> builtFrom;
      if (std::filesystem::exists(module) && builtFrom != stamp.scripts)
      {
        LOG_INFO("Scripts changed since the game was built: rebuilding");
        m_scriptsChanged = true;
      }
    }
    // Even against a baseline: a game module build doesn't rebuild the UI
    // Preview's.
    if (std::exchange(m_uiFirstCheck, false) ? !stamp.ui.empty()
                                             : stamp.ui != m_uiStamp)
      m_uiChanged = true;
    m_scriptsStamp = stamp.scripts;
    m_uiStamp = stamp.ui;
    if (std::exchange(m_scriptsRecheck, false))
      checkScripts(false);
  }
  // UI first: it's quick, and what's being looked at.
  if (m_uiChanged && m_context.project && !m_builder.busy() &&
      !m_openingProject)
  {
    m_uiChanged = false;
    m_builder.start(*m_context.project, ModuleBuilder::Target::UiPreview);
  }
  if (m_scriptsChanged && m_mode == Mode::Edit && m_context.project &&
      !m_builder.busy() && !m_openingProject)
  {
    m_scriptsChanged = false;
    LOG_INFO("Scripts changed: rebuilding");
    requestBuild(false);
  }

  // A scene being opened: a slice of its entities each frame.
  if (m_document && m_document->opening())
    m_document->continueOpening(std::chrono::milliseconds(8));

  // A panel asked to edit a prefab; a prefab's tab was closed (its X, or the
  // dialog asking whether to apply first).
  if (m_context.openPrefab.valid())
    openPrefab(std::exchange(m_context.openPrefab, {}));
  if (!m_context.openText.empty() && m_codePanel)
    m_codePanel->openFile(std::exchange(m_context.openText, {}));
  if (m_context.openMaterial.valid())
    openMaterial(std::exchange(m_context.openMaterial, {}));
  // Material tabs closed (and their changes saved or discarded).
  std::erase_if(m_materialPanels,
                [this](MaterialEditorPanel* panel)
                {
                  if (!panel->finished())
                    return false;
                  std::erase_if(m_panels,
                                [panel](const auto& owned)
                                { return owned.get() == panel; });
                  return true;
                });
  if (Workspace* closing = std::exchange(m_closeNow, nullptr))
    closePrefab(*closing);
  std::vector<Workspace*> closed;
  for (const auto& workspace : m_workspaces)
    if (workspace->prefab.valid() && !workspace->viewport->open() &&
        workspace.get() != m_closing)
      closed.push_back(workspace.get());
  for (Workspace* workspace : closed)
  {
    if (!prefabChanged(*workspace))
    {
      closePrefab(*workspace);
      continue;
    }
    workspace->viewport->open() = true; // until the dialog's answered
    m_closing = workspace;
    m_requestedDialog = Dialog::ClosePrefab;
    break;
  }

  // A viewport focused (clicked, or its tab) edits its scene. During Play the
  // scene being played stays.
  for (const auto& workspace : m_workspaces)
    if (workspace->viewport->takeActivationRequest() && m_mode == Mode::Edit)
      activate(*workspace);
  for (const auto& workspace : m_workspaces)
    workspace->viewport->setUnsaved(workspace->prefab.valid() &&
                                    workspace->history->revision() !=
                                        workspace->appliedRevision);

  // Build results are acted on here, at the start of a frame, never while the
  // UI is mid-draw: a module swap replaces the scene.
  if (const std::optional<bool> built = m_builder.poll();
      built && m_builder.target() == ModuleBuilder::Target::UiPreview)
  {
    if (m_uiPreview)
      m_uiPreview->setBuildFailed(!*built);
    if (*built && m_uiPreview && m_context.project)
      m_uiPreview->loadPreviewModule(
          m_builder.uiPreviewPath(*m_context.project));
    if (std::exchange(m_moduleBuildQueued, false))
      requestBuild(m_playAfterBuild);
  }
  else if (built)
  {
    const bool startPlaying = *built && m_playAfterBuild;
    const bool startExport = *built && m_exportAfterBuild;
    m_playAfterBuild = false;
    m_exportAfterBuild = false;
    if (*built)
      stampModule();
    if (*built && swapModule() && startPlaying)
      play();
    if (startExport)
      exportGame();
  }

  // Back in the editor from elsewhere (an editor, Blender): files may have
  // changed, so the Assets panel reads its folders again.
  const bool focused = SDL_GetKeyboardFocus() != nullptr;
  if (focused && !m_appFocused && m_assetPanel)
    m_assetPanel->requestRefresh();
  if (focused && !m_appFocused)
    checkScripts(false);
  m_appFocused = focused;

  // The viewport sets it again while it draws, if it still has focus.
  m_context.gameHasInput = false;
  m_context.gameHasMouse = false;

  for (const auto& panel : m_panels)
    panel->update(m_context);
}

void EditorApp::onRender()
{
  sfs::IRenderBackend& backend = *renderBackend();
  backend.imguiNewFrame();
  ImGuizmo::BeginFrame();

  drawStatusBar(); // first: it takes its strip before the dockspace fills the
                   // rest
  const ImGuiID dockspace = ImGui::DockSpaceOverViewport();

  // With no saved layout (first run, or imgui.ini deleted) the dockspace starts
  // empty: arrange the panels like a typical engine, once.
  if (!m_layoutChecked)
  {
    m_layoutChecked = true;
    const ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace);
    if (!root || (root->IsLeafNode() && root->Windows.empty()))
      buildDefaultLayout(dockspace);
  }

  handleShortcuts();

  drawMenuBar();
  drawProjectDialogs();

  // Focusing by name also brings the Viewport's tab to the front if another
  // tab (say Code) was showing.
  if (std::exchange(m_context.focusViewport, false))
    ImGui::SetWindowFocus(sceneWorkspace().viewport->title());
  // A new tab exists once it's been drawn.
  if (!m_focusWindow.empty() && ImGui::FindWindowByName(m_focusWindow.c_str()))
    ImGui::SetWindowFocus(std::exchange(m_focusWindow, {}).c_str());
  for (const auto& panel : m_panels)
    panel->draw(m_context);

  backend.imguiRenderDrawData();
}

void EditorApp::buildDefaultLayout(unsigned int dockspaceId)
{
  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(
      dockspaceId, ImGui::GetMainViewport()->WorkSize);

  ImGuiID center = dockspaceId;
  const ImGuiID left = ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Left, 0.18f, nullptr, &center);
  const ImGuiID right = ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Right, 0.25f, nullptr, &center);
  const ImGuiID bottom = ImGui::DockBuilderSplitNode(
      center, ImGuiDir_Down, 0.25f, nullptr, &center);
  ImGuiID rightBottom = 0;
  const ImGuiID rightTop = ImGui::DockBuilderSplitNode(
      right, ImGuiDir_Up, 0.6f, nullptr, &rightBottom);

  ImGui::DockBuilderDockWindow("Hierarchy", left);
  ImGui::DockBuilderDockWindow("Inspector", rightTop);
  ImGui::DockBuilderDockWindow("Scene Settings", rightBottom);
  ImGui::DockBuilderDockWindow("Console", bottom);
  ImGui::DockBuilderDockWindow("Assets", bottom); // tabbed with the console
  ImGui::DockBuilderDockWindow("Viewport", center);
  ImGui::DockBuilderDockWindow("Code", center); // tabbed with the viewport
  ImGui::DockBuilderFinish(dockspaceId);
}

void EditorApp::drawMenuBar()
{
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("File"))
  {
    if (ImGui::MenuItem("New Project..."))
      m_requestedDialog = Dialog::New;
    if (ImGui::MenuItem("Open Project..."))
      m_requestedDialog = Dialog::Open;
    ImGui::Separator();
    drawSceneMenu();
    ImGui::Separator();
    if (ImGui::MenuItem("Export Game...",
                        nullptr,
                        false,
                        m_context.project && !moduleBuilding()))
      m_requestedDialog = Dialog::Export;
    ImGui::Separator();
    if (ImGui::MenuItem("Exit"))
      isRunning = false;
    ImGui::EndMenu();
  }

  drawEditMenu();
  drawScriptsMenu();

  if (ImGui::BeginMenu("Window"))
  {
    for (const auto& panel : m_panels)
      ImGui::MenuItem(panel->title(), nullptr, &panel->open());
    ImGui::EndMenu();
  }

  drawPlayControls();

  ImGui::EndMainMenuBar();
}

void EditorApp::drawStatusBar()
{
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav;
  // One row of frame height, with the same small margin above and below.
  const float margin = 4.0f;
  const float height = ImGui::GetFrameHeight() + 2.0f * margin;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(ImGui::GetStyle().WindowPadding.x, margin));
  const bool open = ImGui::BeginViewportSideBar(
      "##status", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags);
  ImGui::PopStyleVar();
  if (!open)
  {
    ImGui::End();
    return;
  }

  // Each background task: what it is, and how far (negative: unknown).
  struct Task
  {
    std::string label;
    float fraction;
    std::function<void()> stop =
        {}; // offered as a button, if it can be stopped
  };
  std::vector<Task> tasks;
  if (m_openingProject)
    tasks.push_back({"Loading " + m_openingProject->project.name(), -1.0f});
  if (m_document && m_document->opening())
  {
    const auto [done, total] = m_document->openingProgress();
    tasks.push_back(
        {"Loading scene " + std::to_string(done) + "/" + std::to_string(total),
         total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f});
  }
  if (m_builder.busy())
  {
    const int percent = m_builder.percent();
    tasks.push_back({m_builder.target() == ModuleBuilder::Target::UiPreview
                         ? "Building UI previews"
                         : "Building scripts",
                     percent < 0 ? -1.0f : percent / 100.0f,
                     [this] { m_builder.cancel(); }});
  }
  if (m_context.scene)
  {
    const sfs::AssetStore::LoadProgress loads =
        m_context.scene->assetStore().loadProgress();
    if (loads.done < loads.total)
      tasks.push_back(
          {"Loading models " + std::to_string(loads.done) + "/" +
               std::to_string(loads.total),
           static_cast<float>(loads.done) / static_cast<float>(loads.total)});
  }
  if (m_assetPanel && m_assetPanel->refreshing())
    tasks.push_back({"Checking for changed files", -1.0f});
  if (m_assetPanel)
  {
    const Thumbnails::Progress previews = m_assetPanel->previewProgress();
    if (previews.done < previews.total)
      tasks.push_back({"Making previews " + std::to_string(previews.done) +
                           "/" + std::to_string(previews.total),
                       static_cast<float>(previews.done) /
                           static_cast<float>(previews.total)});
  }

  // Every item centred in the row, whatever its height.
  const float rowTop = ImGui::GetCursorPosY();
  const auto centre = [&](float itemHeight)
  {
    ImGui::SetCursorPosY(rowTop +
                         (ImGui::GetFrameHeight() - itemHeight) * 0.5f);
  };
  const float lineHeight = ImGui::GetTextLineHeight();

  if (tasks.empty())
  {
    centre(lineHeight);
    ImGui::TextDisabled("Ready");
  }
  for (std::size_t i = 0; i < tasks.size(); ++i)
  {
    if (i > 0)
      ImGui::SameLine(0.0f, 24.0f);
    centre(lineHeight);
    ImGui::TextUnformatted(tasks[i].label.c_str());
    ImGui::SameLine();
    // A negative fraction animates (ImGui's "busy" bar) while the amount is
    // unknown.
    const float fraction = tasks[i].fraction < 0.0f
                               ? -static_cast<float>(ImGui::GetTime())
                               : tasks[i].fraction;
    ImGui::PushStyleColor(
        ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    centre(lineHeight);
    ImGui::ProgressBar(fraction, ImVec2(140.0f, lineHeight), "");
    ImGui::PopStyleColor();
    if (tasks[i].stop)
    {
      // An x in the bar's right end stops it.
      const ImVec2 barMin = ImGui::GetItemRectMin(),
                   barMax = ImGui::GetItemRectMax();
      const float side = barMax.y - barMin.y;
      const ImVec2 at(barMax.x - side, barMin.y);
      ImGui::SetCursorScreenPos(at);
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::InvisibleButton("stop", ImVec2(side, side)))
        tasks[i].stop();
      const bool hovered = ImGui::IsItemHovered();
      ImGui::SetItemTooltip("Stop");
      ImGui::PopID();

      ImDrawList* draw = ImGui::GetWindowDrawList();
      const ImVec2 middle(at.x + side * 0.5f, at.y + side * 0.5f);
      if (hovered)
        draw->AddCircleFilled(
            middle, side * 0.5f, ImGui::GetColorU32(ImGuiCol_ButtonHovered));
      const float arm = side * 0.2f;
      const ImU32 colour =
          ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
      draw->AddLine(ImVec2(middle.x - arm, middle.y - arm),
                    ImVec2(middle.x + arm, middle.y + arm),
                    colour,
                    1.5f);
      draw->AddLine(ImVec2(middle.x - arm, middle.y + arm),
                    ImVec2(middle.x + arm, middle.y - arm),
                    colour,
                    1.5f);
    }
  }

  // The open project's folder, on the right.
  if (m_context.project)
  {
    const std::string folder = m_context.project->root().string();
    ImGui::SameLine(ImGui::GetWindowWidth() -
                    ImGui::CalcTextSize(folder.c_str()).x -
                    ImGui::GetStyle().WindowPadding.x);
    centre(lineHeight);
    ImGui::TextDisabled("%s", folder.c_str());
  }
  ImGui::End();
}

void EditorApp::drawSceneMenu()
{
  // Scenes live in the project's scenes folder, so they need a project.
  const bool haveProject = m_context.project.has_value();

  if (ImGui::MenuItem("New Scene", nullptr, false, m_mode == Mode::Edit))
  {
    activate(sceneWorkspace());
    m_document->newScene();
    m_history.clear();
    m_context.selection = {};
    updateWindowTitle();
  }

  if (ImGui::BeginMenu("Open Scene", haveProject))
  {
    const std::vector<std::filesystem::path> files =
        sceneFiles(*m_context.project);
    for (const std::filesystem::path& file : files)
      if (ImGui::MenuItem(file.stem().string().c_str(),
                          nullptr,
                          file == m_document->path()))
        openScene(file);
    if (files.empty())
      ImGui::TextDisabled("No scenes in %s",
                          m_context.project->scenesFolder().string().c_str());
    ImGui::EndMenu();
  }

  // Saving during Play would write the play state over the scene.
  const bool canSave = haveProject && m_mode == Mode::Edit;
  if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, canSave))
    saveScene();
  if (ImGui::MenuItem("Save Scene As...", nullptr, false, canSave))
    m_requestedDialog = Dialog::SaveSceneAs;
}

void EditorApp::handleShortcuts()
{
  constexpr ImGuiInputFlags kGlobal = ImGuiInputFlags_RouteGlobal;

  // Typing in the code editor: its keys (Ctrl+S, Ctrl+P, ...) are Neovim's.
  if (m_context.keyboardCaptured)
    return;

  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, kGlobal))
    saveScene();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, kGlobal))
    requestBuild(false);
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, kGlobal))
    m_mode == Mode::Playing ? stop() : requestBuild(true);

  // Edit shortcuts belong to text fields while one is being typed in.
  if (ImGui::GetIO().WantTextInput)
    return;
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, kGlobal))
    m_context.history->undo();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, kGlobal) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, kGlobal))
    m_context.history->redo();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, kGlobal))
    duplicateSelection();
  if (ImGui::Shortcut(ImGuiKey_Delete, kGlobal))
    deleteSelection();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, kGlobal))
    alignSelectionWithView();
}

void EditorApp::drawEditMenu()
{
  if (!ImGui::BeginMenu("Edit"))
    return;
  // The Assets panel's file operations, or a material's graph, while it was
  // the last panel used; otherwise the scene's.
  if (MaterialEditorPanel* material = materialPanelWithEdit())
  {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, material->canUndo()))
      material->undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, material->canRedo()))
      material->redo();
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
      material->duplicateSelection();
    if (ImGui::MenuItem("Delete", "Del"))
      material->deleteSelection();
  }
  else if (m_assetPanel && m_assetPanel->ownsEdit())
  {
    const std::string undo = "Undo " + m_assetPanel->undoLabel(),
                      redo = "Redo " + m_assetPanel->redoLabel();
    if (ImGui::MenuItem(m_assetPanel->canUndo() ? undo.c_str() : "Undo",
                        "Ctrl+Z",
                        false,
                        m_assetPanel->canUndo()))
      m_assetPanel->undo(m_context);
    if (ImGui::MenuItem(m_assetPanel->canRedo() ? redo.c_str() : "Redo",
                        "Ctrl+Y",
                        false,
                        m_assetPanel->canRedo()))
      m_assetPanel->redo(m_context);
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
      m_assetPanel->duplicateSelected(m_context);
    if (ImGui::MenuItem("Delete", "Del"))
      m_assetPanel->deleteSelected(m_context);
  }
  else
  {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_context.history->canUndo()))
      m_context.history->undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_context.history->canRedo()))
      m_context.history->redo();
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_context.hasSelection()))
      duplicateSelection();
    if (ImGui::MenuItem("Delete", "Del", false, m_context.hasSelection()))
      deleteSelection();
  }
  ImGui::Separator();
  const bool canAlign =
      m_mode == Mode::Edit && m_context.hasSelection() &&
      m_context.selection.hasComponent<sfs::TransformComponent>();
  if (ImGui::MenuItem("Align With View", "Ctrl+Shift+F", false, canAlign))
    alignSelectionWithView();
  ImGui::SetItemTooltip(
      "Move the selection to where the viewport is looking from");
  if (ImGui::MenuItem("Align View With Selected", nullptr, false, canAlign))
    alignViewWithSelection();
  ImGui::SetItemTooltip(
      "Look through the selection (a camera's view, while editing)");
  ImGui::EndMenu();
}

void EditorApp::duplicateSelection()
{
  if (!m_context.hasSelection())
    return;

  // Each selected branch once (not again inside another selected one).
  const std::vector<sfs::Entity> selected = m_context.selected();
  std::vector<sfs::Entity> branches;
  for (const sfs::Entity& entity : selected)
    if (std::none_of(selected.begin(),
                     selected.end(),
                     [&](const sfs::Entity& other)
                     { return other != entity && sfs::hierarchy::isAncestor(other, entity); }))
      branches.push_back(entity);

  m_context.history->change(
      [&]
      {
        const sfs::SceneSerializer serializer = m_context.scene->serializer();
        // Each whole branch, beside its original (same parent); the copies
        // selected.
        std::vector<sfs::Entity> copies;
        for (const sfs::Entity& original : branches)
        {
          sfs::Entity copy = serializer.loadEntity(
              serializer.saveEntity(*m_context.scene, original),
              *m_context.scene,
              sfs::hierarchy::parentOf(original));
          if (copy.hasComponent<sfs::NameComponent>())
            copy.getComponent<sfs::NameComponent>().name += " (copy)";
          copies.push_back(copy);
        }
        m_context.scene->flushEntities();
        m_context.selection = copies.front();
        m_context.selectionGroup = copies;
      });
}

void EditorApp::deleteSelection()
{
  if (!m_context.hasSelection())
    return;

  // Cleared inside the change, so undo brings the entity back selected.
  m_context.history->change(
      [&]
      {
        const std::vector<sfs::Entity> doomed = m_context.selected();
        m_context.selection = {};
        m_context.selectionGroup.clear();
        for (const sfs::Entity& entity : doomed)
          if (entity.isAlive())
            m_context.scene->destroyBranch(entity);
      });
}

void EditorApp::alignSelectionWithView()
{
  if (m_mode != Mode::Edit || !m_context.hasSelection() ||
      !m_context.selection.hasComponent<sfs::TransformComponent>())
    return;

  m_context.history->change(
      [&]
      {
        const sfs::Camera3D& view = m_context.render->camera();
        sfs::Entity entity = m_context.selection;
        const glm::vec3 scale =
            entity.getComponent<sfs::TransformComponent>().scale;
        sfs::hierarchy::setWorldMatrix(
            entity,
            sfs::TransformComponent(view.position(), view.orientation())
                .matrix());
        entity.getComponent<sfs::TransformComponent>().scale = scale;
        if (entity.hasComponent<sfs::CameraComponent>())
          entity.getComponent<sfs::CameraComponent>().focusDistance =
              view.distance;
      });
}

void EditorApp::alignViewWithSelection()
{
  if (!m_context.hasSelection() ||
      !m_context.selection.hasComponent<sfs::TransformComponent>())
    return;

  // Not an edit: the editor's view isn't part of the scene's undo history.
  const sfs::Entity entity = m_context.selection;
  const glm::mat4 world = sfs::hierarchy::worldMatrix(entity);
  sfs::Camera3D& view = m_context.render->camera();
  if (entity.hasComponent<sfs::CameraComponent>())
    entity.getComponent<sfs::CameraComponent>().applyTo(view, world);
  else
    view.setPose(glm::vec3(world[3]), glm::vec3(world[0]), view.distance);
}

void EditorApp::drawScriptsMenu()
{
  using scriptTemplates::Kind;
  if (!ImGui::BeginMenu("Scripts", m_context.project.has_value()))
    return;

  for (const Kind kind : {Kind::GameObject,
                          Kind::System,
                          Kind::Component,
                          Kind::ScriptableObject})
  {
    const std::string label =
        std::string("New ") + scriptTemplates::label(kind) + "...";
    if (ImGui::MenuItem(label.c_str()))
    {
      m_dialogScriptKind = kind;
      m_dialogScriptFolder.clear(); // the scripts folder itself
      m_dialogScriptName.clear();
      m_requestedDialog = Dialog::NewScript;
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Build", "Ctrl+B", false, !moduleBuilding()))
    requestBuild(false);
  ImGui::EndMenu();
}

void EditorApp::drawPlayControls()
{
  // Centred in the menu bar, like most engines.
  const char* label = moduleBuilding()          ? "Building..."
                      : m_mode == Mode::Playing ? "Stop"
                                                : "Play";
  const float width = ImGui::CalcTextSize("Building...").x + 24.0f;
  ImGui::SameLine((ImGui::GetWindowWidth() - width) * 0.5f);

  ImGui::BeginDisabled(moduleBuilding() || !m_context.project);
  // Captured once: pressing Stop changes the mode before the colour is popped.
  const bool playing = m_mode == Mode::Playing;
  if (playing)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.25f, 0.20f, 1.0f));
  if (ImGui::Button(label, ImVec2(width, 0.0f)))
    playing ? stop() : requestBuild(true);
  if (playing)
    ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Build the scripts and play the scene (Ctrl+P)");
}

void EditorApp::drawProjectDialogs()
{
  // Popups must open from the same ID stack they're drawn in, so the menu
  // only requests them and they open here.
  if (m_requestedDialog == Dialog::Open)
    ImGui::OpenPopup(kOpenDialog);
  else if (m_requestedDialog == Dialog::New)
    ImGui::OpenPopup(kNewDialog);
  else if (m_requestedDialog == Dialog::SaveSceneAs)
    ImGui::OpenPopup(kSaveSceneDialog);
  else if (m_requestedDialog == Dialog::NewScript)
    ImGui::OpenPopup(kNewScriptDialog);
  else if (m_requestedDialog == Dialog::Export && m_context.project)
  {
    // Defaults: the project's start scene (else the open one), into builds/.
    const Project& project = *m_context.project;
    m_exportFolder =
        (project.exportFolder() / m_environment.platformName()).string();
    if (!m_lastExportFolder.empty())
      m_exportFolder = m_lastExportFolder;
    m_exportStartScene = project.startScene().generic_string();
    if (m_exportStartScene.empty() && !m_document->path().empty())
      m_exportStartScene =
          (std::filesystem::path("scenes") / m_document->path().filename())
              .generic_string();
    ImGui::OpenPopup(kExportDialog);
  }
  else if (m_requestedDialog == Dialog::ClosePrefab)
    ImGui::OpenPopup(kClosePrefabDialog);
  m_requestedDialog = Dialog::None;
  drawClosePrefabDialog();

  if (ImGui::BeginPopupModal(
          kNewScriptDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("New %s (class name, also the file name):",
                scriptTemplates::label(m_dialogScriptKind));
    if (m_context.project && !m_dialogScriptFolder.empty())
    {
      const std::filesystem::path root = m_context.project->root();
      ImGui::TextDisabled(
          "In %s",
          paths::relative(m_dialogScriptFolder, root).value_or("").c_str());
    }
    ImGui::SetNextItemWidth(320.0f);
    widgets::inputText("##scriptName", m_dialogScriptName);
    const bool valid = scriptTemplates::isValidName(m_dialogScriptName);
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Create") && m_context.project)
    {
      const std::filesystem::path scripts = m_context.project->scriptsFolder();
      if (const auto created = scriptTemplates::create(
              m_dialogScriptKind,
              m_dialogScriptName,
              scripts,
              m_dialogScriptFolder.empty() ? scripts : m_dialogScriptFolder))
      {
        m_codePanel->openFile(*created);
        m_assetPanel->requestRefresh();
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  drawExportDialog();

  if (ImGui::BeginPopupModal(
          kSaveSceneDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::TextUnformatted(
        "Scene name (saved in the project's scenes folder):");
    ImGui::SetNextItemWidth(320.0f);
    widgets::inputText("##sceneName", m_dialogSceneName);
    if (ImGui::Button("Save") && !m_dialogSceneName.empty() &&
        m_context.project)
    {
      std::filesystem::path path =
          m_context.project->scenesFolder() / m_dialogSceneName;
      path.replace_extension(SceneDocument::kExtension);
      m_document->saveAs(path);
      m_assetPanel->requestRefresh();
      updateWindowTitle();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopupModal(
          kOpenDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::TextUnformatted("Project folder (containing project.sfsproj):");
    ImGui::SetNextItemWidth(480.0f);
    widgets::inputText("##path", m_dialogPath);
    if (ImGui::Button("Open"))
    {
      openProject(m_dialogPath);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopupModal(
          kNewDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::SetNextItemWidth(480.0f);
    widgets::inputText("Name", m_dialogName);
    ImGui::SetNextItemWidth(480.0f);
    widgets::inputText("Folder", m_dialogPath);
    if (ImGui::Button("Create") && !m_dialogName.empty() &&
        !m_dialogPath.empty())
    {
      createProject(m_dialogPath, m_dialogName);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void EditorApp::openProject(const std::filesystem::path& folder)
{
  if (m_openingProject)
    return; // one at a time
  std::optional<Project> project = Project::open(folder);
  if (!project)
    return;
  if (m_mode == Mode::Playing)
    stop();
  // Prefab and material tabs belong to the project before: their changes
  // are applied.
  while (m_workspaces.size() > 1)
  {
    Workspace& prefab = *m_workspaces.back();
    if (prefabChanged(prefab))
      applyPrefab(prefab);
    closePrefab(prefab);
  }
  for (MaterialEditorPanel* panel : m_materialPanels)
  {
    if (panel->unsaved())
      panel->save(m_context);
    std::erase_if(
        m_panels, [panel](const auto& owned) { return owned.get() == panel; });
  }
  m_materialPanels.clear();
  project->ensureBuildFiles();
  std::error_code error;
  std::filesystem::create_directories(project->assetsFolder(), error);
  std::filesystem::create_directories(project->prefabsFolder(), error);

  // The project's asset catalogue, scanned on a worker (giving new files
  // their .meta); nothing else touches it until it's handed over.
  auto assets = std::async(std::launch::async,
                           [root = project->root()]
                           {
                             return sfs::AssetDatabase(
                                 root,
                                 sfs::AssetDatabase::Mode::ReadWrite,
                                 sfs::AssetDatabase::projectFolders());
                           });
  m_openingProject = OpeningProject{std::move(*project), std::move(assets)};
}

void EditorApp::createProject(const std::filesystem::path& folder,
                              const std::string& name)
{
  if (Project::create(folder, name))
    openProject(folder);
}

void EditorApp::finishOpeningProject()
{
  OpeningProject opening = std::move(*m_openingProject);
  m_openingProject.reset();
  m_context.project = std::move(opening.project);

  m_context.assets = nullptr;
  m_context.selectedAsset = {};
  setAssetDatabase(nullptr);
  sfs::scriptableObjects().setCatalogue(nullptr);
  m_assets.emplace(opening.assets.get());
  m_context.assets = &*m_assets;
  setAssetDatabase(&*m_assets);
  sfs::scriptableObjects().setCatalogue(&*m_assets);

  m_scriptsStamp.clear();
  m_uiStamp.clear();
  m_uiFirstCheck = true; // its UI Preview module is built, if it has UI
  m_moduleFirstCheck = true;
  if (m_uiPreview)
    m_uiPreview->dropPreviewModule(); // the last project's
  checkScripts(true);

  // Load the last built module first, so scenes using its components load.
  loadExistingModule();

  // Start on the project's main scene, else its first, else a new one.
  const std::vector<std::filesystem::path> files =
      sceneFiles(*m_context.project);
  const auto main = std::find_if(files.begin(),
                                 files.end(),
                                 [](const std::filesystem::path& file)
                                 { return file.stem() == "main"; });

  if (main != files.end())
    openScene(*main);
  else if (!files.empty())
    openScene(files.front());
  else
  {
    m_document->newScene();
    m_history.clear();
    m_context.selection = {};
  }
  updateWindowTitle();
}

void EditorApp::openScene(const std::filesystem::path& path)
{
  if (m_mode == Mode::Playing)
    stop();
  activate(sceneWorkspace());
  m_document->beginOpen(path); // its entities come in over the next frames
  m_history.clear();
  m_context.selection = {};
  updateWindowTitle();
}

void EditorApp::saveScene()
{
  if (!m_context.project || m_mode != Mode::Edit)
    return;
  if (editingPrefab())
  {
    applyPrefab(*m_active); // Ctrl+S saves what's being edited
    return;
  }
  if (m_document->hasPath())
    m_document->save();
  else
    m_requestedDialog = Dialog::SaveSceneAs; // first save picks a name
}

void EditorApp::updateWindowTitle()
{
  std::string title = "SfsEditor";
  if (m_context.project)
  {
    std::string scene =
        m_document->hasPath() ? m_document->path().stem().string() : "Untitled";
    if (editingPrefab())
      if (const sfs::AssetRecord* record =
              m_assets ? m_assets->find(m_active->prefab) : nullptr)
        scene = "Prefab " + record->path.filename().string();
    title = m_context.project->name() + " - " + scene +
            (m_mode == Mode::Playing ? " [Playing]" : "") + " - SfsEditor";
  }
  SDL_SetWindowTitle(window, title.c_str());
}

void EditorApp::requestBuild(bool thenPlay)
{
  if (!m_context.project)
    return;
  if (m_builder.busy())
  {
    // Already building (e.g. after a script was saved): Play once it's done.
    // A UI Preview build is quick: build the module after it.
    m_playAfterBuild |= thenPlay;
    if (m_builder.target() == ModuleBuilder::Target::UiPreview)
      m_moduleBuildQueued = true;
    return;
  }
  if (m_mode == Mode::Playing)
    stop(); // rebuilding replaces the module the play session runs
  if (m_builder.start(*m_context.project))
  {
    m_playAfterBuild = thenPlay;
    checkScripts(true); // what this build is of: not a change to rebuild for
  }
}

namespace
{
// Floats as the scene files keep them (single precision), so JSON made in
// memory compares equal to the same JSON read back from a file.
nlohmann::json asStored(nlohmann::json json)
{
  if (json.is_number_float())
    return static_cast<double>(static_cast<float>(json.get<double>()));
  if (json.is_structured())
    for (auto& item : json)
      item = asStored(std::move(item));
  return json;
}

// A selected entity by its position in save order (as EditorHistory keeps
// it): a reloaded scene's entities come back in that order.
std::optional<std::size_t> orderOf(EditorScene& scene,
                                   const sfs::Entity& entity)
{
  if (!entity.isValid() || !entity.isAlive())
    return std::nullopt;
  const std::vector<sfs::Entity> order =
      sfs::hierarchy::EntityTree(scene.entities()).preOrder();
  const auto it = std::find(order.begin(), order.end(), entity);
  if (it == order.end())
    return std::nullopt;
  return static_cast<std::size_t>(it - order.begin());
}

sfs::Entity atOrder(EditorScene& scene, std::optional<std::size_t> index)
{
  if (!index)
    return {};
  const std::vector<sfs::Entity> order =
      sfs::hierarchy::EntityTree(scene.entities()).preOrder();
  return *index < order.size() ? order[*index] : sfs::Entity();
}

// A prefab's content alone in a scene: its root no longer an instance, so it
// and everything below it are edited as themselves, keeping their prefab ids.
sfs::Entity loadPrefabContent(EditorScene& scene,
                              SceneDocument& document,
                              const sfs::AssetId& prefab)
{
  document.restore(nlohmann::json{
      {"formatVersion", 2}, {"entities", nlohmann::json::array()}});
  sfs::Entity root =
      scene.serializer().instantiate(prefabs::nameOf(prefab), scene);
  scene.flushEntities();
  if (root.isValid() && root.hasComponent<sfs::PrefabInstanceComponent>())
    root.removeComponent<sfs::PrefabInstanceComponent>();
  return root;
}
} // namespace

EditorScene* EditorApp::makeScene(bool forPrefab)
{
  EditorScene* scene = sceneManager.createScene<EditorScene>();
  sfs::RenderSystem3D& render = scene->renderSystem();
  // While editing, the viewport is the editor's own free view.
  render.setFollowSceneCamera(false);
  render.meshes().setMissingExpected(m_awaitingModule);
  if (forPrefab)
  {
    // On a grid against a plain grey, like a modelling tool.
    render.withModule<GridModule>(render.meshes());
    sfs::SceneLighting3D lighting = render.lighting();
    lighting.skyColor = glm::vec3(0.075f, 0.08f, 0.078f);
    render.setLighting(lighting);
  }
  return scene;
}

void EditorApp::bindContext()
{
  m_context.scene = m_active ? m_active->scene : nullptr;
  m_context.render =
      m_context.scene ? &m_context.scene->renderSystem() : nullptr;
  m_context.document = m_active ? m_active->document : nullptr;
  m_context.history = m_active ? m_active->history : &m_history;
}

void EditorApp::activate(Workspace& workspace)
{
  if (&workspace == m_active)
    return;
  if (m_active)
  {
    m_active->selection = m_context.selection;
    m_active->viewport->setActive(false);
  }
  m_active = &workspace;
  m_context.selection = workspace.selection;
  m_context.focusRequest = {};
  bindContext();
  sceneManager.load(workspace.scene->id());
  workspace.viewport->setActive(true);
  updateWindowTitle();
}

void EditorApp::openPrefab(const sfs::AssetId& prefab)
{
  const sfs::AssetRecord* record = m_assets ? m_assets->find(prefab) : nullptr;
  if (!record || record->kind != sfs::AssetKind::Prefab)
    return;
  if (m_mode == Mode::Playing)
    stop();

  // Already open: bring its tab forward.
  for (const auto& workspace : m_workspaces)
    if (workspace->prefab == prefab)
    {
      workspace->viewport->open() = true;
      m_focusWindow = workspace->viewport->title();
      activate(*workspace);
      return;
    }

  auto& workspace = *m_workspaces.emplace_back(std::make_unique<Workspace>());
  workspace.prefab = prefab;
  workspace.scene = makeScene(true);
  workspace.ownDocument = std::make_unique<SceneDocument>(*workspace.scene);
  workspace.ownHistory = std::make_unique<EditorHistory>(m_context);
  workspace.document = workspace.ownDocument.get();
  workspace.history = workspace.ownHistory.get();

  // A tab named after the prefab, beside the scene's.
  auto& backend = static_cast<sfs::WebGpuRenderBackend&>(*renderBackend());
  auto viewport = std::make_unique<ViewportPanel>(
      backend, record->path.stem().string() + "###prefab-" + prefab.toString());
  viewport->dockWith(sceneWorkspace().viewport->title());
  viewport->setToolbarExtras(
      [this, &workspace](EditorContext&)
      {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(
            ImGui::ColorConvertU32ToFloat4(theme::kAccent), "Prefab");
        ImGui::SameLine();
        ImGui::BeginDisabled(workspace.history->revision() ==
                             workspace.appliedRevision);
        if (ImGui::Button("Apply"))
          applyPrefab(workspace);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Write the prefab (Ctrl+S): every copy of it "
                              "updates, except where a copy\n"
                              "has changed something itself");
      });
  workspace.viewport = viewport.get();
  m_focusWindow = viewport->title();
  m_panels.push_back(std::move(viewport));

  const sfs::Entity root =
      loadPrefabContent(*workspace.scene, *workspace.document, prefab);
  if (!root.isValid())
  {
    LOG_ERROR("Can't open the prefab " + record->path.generic_string());
    closePrefab(workspace);
    return;
  }
  workspace.selection = root;
  activate(workspace);
  m_context.selectedAsset = {};
  m_context.focusRequest = root;
  LOG_INFO("Editing the prefab " + record->path.generic_string());
}

bool EditorApp::applyPrefab(Workspace& workspace)
{
  const sfs::AssetRecord* record =
      m_assets ? m_assets->find(workspace.prefab) : nullptr;
  if (!record)
    return false;
  const sfs::hierarchy::EntityTree tree(workspace.scene->entities());
  if (tree.roots().size() != 1)
  {
    LOG_ERROR("A prefab has one top object: put the others under it to apply");
    return false;
  }
  const nlohmann::json prefab = workspace.scene->serializer().savePrefab(
      *workspace.scene, tree.roots().front(), true);
  const bool written = rewritePrefab(
      workspace.prefab,
      [&]
      {
        if (!sfs::SceneSerializer::writeJsonFile(
                prefab, m_assets->absolutePath(*record)))
          return false;
        workspace.scene->assetStore().forgetPrefab(record->id);
        return true;
      },
      &workspace);
  if (!written)
    return false;
  workspace.appliedRevision = workspace.history->revision();
  LOG_INFO("Applied the prefab " + record->path.generic_string());
  return true;
}

bool EditorApp::prefabChanged(Workspace& workspace)
{
  const sfs::AssetRecord* record =
      m_assets ? m_assets->find(workspace.prefab) : nullptr;
  if (!record)
    return false; // nothing left to apply it to
  const sfs::hierarchy::EntityTree tree(workspace.scene->entities());
  if (tree.roots().size() != 1)
    return true;
  const std::optional<nlohmann::json> file =
      sfs::SceneSerializer::readJsonFile(m_assets->absolutePath(*record));
  return !file ||
         asStored(*file) != asStored(workspace.scene->serializer().savePrefab(
                                *workspace.scene, tree.roots().front(), true));
}

void EditorApp::closePrefab(Workspace& workspace)
{
  if (m_active == &workspace)
    activate(sceneWorkspace());
  if (m_closing == &workspace)
    m_closing = nullptr;
  if (workspace.scene)
    sceneManager.destroyScene(workspace.scene->id());
  std::erase_if(m_panels,
                [&](const auto& panel)
                { return panel.get() == workspace.viewport; });
  std::erase_if(
      m_workspaces, [&](const auto& open) { return open.get() == &workspace; });
}

bool EditorApp::rewritePrefab(const sfs::AssetId& prefab,
                              const std::function<bool()>& write,
                              const Workspace* source)
{
  // Each scene that may show the prefab, as it is now (with what's selected),
  // to reload once the prefab is rewritten: its instances then take the new
  // prefab, keeping their own changes.
  struct Reload
  {
    Workspace* workspace;
    std::optional<nlohmann::json> snapshot; // none: reload the prefab itself
    std::optional<std::size_t> selection;
  };
  std::vector<Reload> reloads;
  for (const auto& workspace : m_workspaces)
  {
    if (workspace.get() == source || !workspace->scene)
      continue;
    const sfs::Entity& selected = workspace.get() == m_active
                                      ? m_context.selection
                                      : workspace->selection;
    Reload reload{
        workspace.get(), std::nullopt, orderOf(*workspace->scene, selected)};
    if (workspace->prefab == prefab)
    {
      if (workspace->history->revision() != workspace->appliedRevision)
      {
        LOG_INFO("The prefab's tab has changes of its own, so it keeps them "
                 "(Apply there would replace this)");
        continue;
      }
    }
    else
    {
      bool hasInstances = false;
      for (const sfs::Entity& entity : workspace->scene->entities())
        hasInstances |= entity.hasComponent<sfs::PrefabInstanceComponent>();
      if (!hasInstances)
        continue;
      reload.snapshot = workspace->document->snapshot();
    }
    reloads.push_back(std::move(reload));
  }

  if (!write())
    return false;

  for (Reload& reload : reloads)
  {
    Workspace& workspace = *reload.workspace;
    // Reloading is about the scene's contents, not where it's seen from.
    const sfs::Camera3D camera = workspace.scene->renderSystem().camera();
    if (reload.snapshot)
      workspace.document->restore(*reload.snapshot);
    else
    {
      loadPrefabContent(
          *workspace.scene, *workspace.document, workspace.prefab);
      workspace.history->clear();
      workspace.appliedRevision = workspace.history->revision();
    }
    workspace.scene->renderSystem().camera() = camera;
    (&workspace == m_active ? m_context.selection : workspace.selection) =
        atOrder(*workspace.scene, reload.selection);
  }
  return true;
}

void EditorApp::openMaterial(const sfs::AssetId& material)
{
  const sfs::AssetRecord* record =
      m_assets ? m_assets->find(material) : nullptr;
  if (!record || record->kind != sfs::AssetKind::Material)
    return;

  // Already open: bring its tab forward.
  for (MaterialEditorPanel* panel : m_materialPanels)
    if (panel->material() == material)
    {
      panel->open() = true;
      m_focusWindow = panel->title();
      return;
    }

  auto& backend = static_cast<sfs::WebGpuRenderBackend&>(*renderBackend());
  auto panel =
      std::make_unique<MaterialEditorPanel>(backend.context(), *record);
  if (!panel->load(m_context))
    return;
  // Beside the other open materials; the first below the scene's viewport,
  // which stays in view to show the edits.
  const auto other =
      std::find_if(m_materialPanels.begin(),
                   m_materialPanels.end(),
                   [](MaterialEditorPanel* open) { return open->open(); });
  if (other != m_materialPanels.end())
    panel->dockWith((*other)->title());
  else
    panel->dockBelow(sceneWorkspace().viewport->title());
  m_focusWindow = panel->title();
  m_materialPanels.push_back(panel.get());
  m_panels.push_back(std::move(panel));
  LOG_INFO("Editing the material " + record->path.generic_string());
}

MaterialEditorPanel* EditorApp::materialPanelWithEdit() const
{
  for (MaterialEditorPanel* panel : m_materialPanels)
    if (panel->open() && panel->ownsEdit())
      return panel;
  return nullptr;
}

void EditorApp::drawClosePrefabDialog()
{
  if (!ImGui::BeginPopupModal(
          kClosePrefabDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  const sfs::AssetRecord* record =
      m_closing && m_assets ? m_assets->find(m_closing->prefab) : nullptr;
  if (!record)
  {
    m_closing = nullptr;
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::Text("Apply your changes to %s before closing it?",
              record->path.filename().string().c_str());
  ImGui::TextDisabled("Applying updates every copy of it.");
  ImGui::Spacing();
  if (ImGui::Button("Apply"))
  {
    if (applyPrefab(*m_closing))
    {
      m_closeNow = std::exchange(m_closing, nullptr);
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Discard"))
  {
    m_closeNow = std::exchange(m_closing, nullptr);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel"))
  {
    m_closing = nullptr;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void EditorApp::destroyScenes()
{
  // The preview's copy of a layer is the game module's too.
  if (m_uiPreview)
    m_uiPreview->dropObjects();
  m_context.selection = {};
  for (const auto& workspace : m_workspaces)
  {
    workspace->scene = nullptr;
    workspace->selection = {};
  }
  bindContext();
  sceneManager.destroyAll();
}

void EditorApp::createScenes()
{
  for (const auto& workspace : m_workspaces)
  {
    workspace->scene = makeScene(workspace->prefab.valid());
    if (workspace->ownDocument)
      workspace->ownDocument->rebind(*workspace->scene);
    else if (m_document)
      m_document->rebind(*workspace->scene);
    else
      m_document.emplace(*workspace->scene);
    if (!workspace->ownDocument)
      workspace->document = &*m_document;
  }
  bindContext();
  sceneManager.load(m_active->scene->id());
  m_active->viewport->setActive(true);
}

void EditorApp::replaceMainScene()
{
  Workspace& scene = sceneWorkspace();
  EditorScene* old = scene.scene;
  if (m_active == &scene)
    m_context.selection = {};
  scene.selection = {};
  scene.scene = makeScene(false);
  m_document->rebind(*scene.scene);
  if (m_active == &scene)
    sceneManager.load(scene.scene->id());
  sceneManager.destroyScene(old->id());
  bindContext();
}

bool EditorApp::swapModule()
{
  const std::filesystem::path module = m_builder.modulePath(*m_context.project);
  const std::string moduleName = m_context.project->moduleName();
  std::vector<nlohmann::json> snapshots;
  for (const auto& workspace : m_workspaces)
    snapshots.push_back(workspace->document->snapshot());
  // The module's types as they were, to tell what the new build changed.
  const std::vector<fieldMigration::Layout> before =
      fieldMigration::layoutsOf(sfs::reflect::types(), moduleName);

  // The scenes can't outlive the code behind their components: tear them
  // down, swap the module, then rebuild them from the snapshots.
  destroyScenes();
  sfs::scriptableObjects()
      .clear(); // objects of the module's types go before its code does
  m_module.unload();
  const bool loaded =
      m_module.load(module, moduleName, sfs::ModuleHost::Load::FromCopy);
  m_awaitingModule = !loaded;

  // Fields renamed or removed: the scenes (their history too) and the
  // project's files follow, so values move to the new names.
  const fieldMigration::Changes changes = fieldMigration::diff(
      before, fieldMigration::layoutsOf(sfs::reflect::types(), moduleName));
  if (!changes.empty())
  {
    LOG_INFO("Code changed (" + changes.describe() +
             "): updating scenes and prefabs");
    for (nlohmann::json& snapshot : snapshots)
      fieldMigration::apply(changes, snapshot);
    for (const auto& workspace : m_workspaces)
      workspace->history->rewrite([&](nlohmann::json& scene)
                                  { fieldMigration::apply(changes, scene); });
    migrateProjectFiles(changes);
  }

  createScenes();
  for (std::size_t i = 0; i < m_workspaces.size(); ++i)
    m_workspaces[i]->document->restore(snapshots[i]);
  return loaded;
}

void EditorApp::pathMoved(const std::filesystem::path& from,
                          const std::filesystem::path& to)
{
  // What moved, as it now is: the path itself, or something inside it.
  const auto follow = [&](const std::filesystem::path& path)
      -> std::optional<std::filesystem::path>
  {
    if (!paths::within(path, from))
      return std::nullopt;
    return paths::moved(path, from, to);
  };
  // Open scenes save to where their files are now.
  for (const auto& workspace : m_workspaces)
    if (workspace->document && workspace->document->hasPath())
      if (const auto now = follow(workspace->document->path()))
        workspace->document->setPath(*now);
  // The start scene, by its new path.
  if (m_context.project && !m_context.project->startScene().empty())
    if (const auto now =
            follow(m_context.project->root() / m_context.project->startScene()))
      m_context.project->setStartScene(*now);
  updateWindowTitle();
}

void EditorApp::migrateProjectFiles(const fieldMigration::Changes& changes)
{
  const Project& project = *m_context.project;
  int updated = 0;
  for (const std::filesystem::path& folder : {project.scenesFolder(),
                                              project.prefabsFolder(),
                                              project.assetsFolder()})
  {
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, error);
         !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error))
    {
      const std::filesystem::path& file = it->path();
      const std::filesystem::path extension = file.extension();
      const bool scriptableObject =
          extension == sfs::ScriptableObjects::kExtension;
      if (extension != SceneDocument::kExtension && extension != ".prefab" &&
          !scriptableObject)
        continue;
      std::optional<nlohmann::json> data =
          sfs::SceneSerializer::readJsonFile(file);
      if (!data ||
          !(scriptableObject
                ? fieldMigration::applyToScriptableObject(changes, *data)
                : fieldMigration::apply(changes, *data)) ||
          !sfs::SceneSerializer::writeJsonFile(*data, file))
        continue;
      ++updated;
      // A prefab read before the change: read it again. (Through the
      // backend's store: the scenes may be torn down just now.)
      if (m_assets)
        if (const sfs::AssetRecord* record =
                m_assets->findByPath(file.lexically_relative(m_assets->root())))
          renderBackend()->sceneServices()->assetStore.forgetPrefab(record->id);
    }
  }
  if (updated > 0)
    LOG_INFO("Updated " + std::to_string(updated) +
             " scene, prefab and scriptable object file" +
             (updated == 1 ? "" : "s"));
}

void EditorApp::checkScripts(bool baseline)
{
  if (!m_context.project)
    return;
  if (m_scriptsCheck.valid())
  {
    // One under way: a baseline makes it the new normal; a change asks for
    // another after it (the files may have changed since it read them).
    m_scriptsCheckIsBaseline |= baseline;
    m_scriptsRecheck |= !baseline;
    return;
  }
  m_scriptsCheckIsBaseline = baseline;
  // Contents, not file times (which can't be read reliably on a WSL share).
  m_scriptsCheck = std::async(
      std::launch::async,
      [folder = m_context.project->scriptsFolder()]
      {
        // FNV-1a: the UI files' apart (they build the UI Preview's module).
        uint64_t scripts = 14695981039346656037ull;
        uint64_t ui = scripts;
        bool anyUi = false;
        const auto mix = [](uint64_t& hash, std::string_view bytes)
        {
          for (const char c : bytes)
            hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        };
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (auto it =
                 std::filesystem::recursive_directory_iterator(folder, error);
             !error && it != std::filesystem::recursive_directory_iterator();
             it.increment(error))
          if (it->is_regular_file(error))
            files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& file : files)
        {
          const std::string name = file.filename().string();
          const bool isUi =
              name.ends_with(".ui.h") || name.ends_with(".ui.cpp");
          anyUi |= isUi;
          uint64_t& hash = isUi ? ui : scripts;
          mix(hash, file.generic_string());
          std::ifstream in(file, std::ios::binary);
          mix(hash, std::string(std::istreambuf_iterator<char>(in), {}));
        }
        return ScriptsStamp{std::to_string(scripts),
                            anyUi ? std::to_string(ui) : std::string()};
      });
}

namespace
{
// Which build of the engine this editor runs: a hash of the engine
// libraries beside it, to save and compare. (Their contents, not file times:
// those can't be read on a WSL share seen from Windows.)
std::string engineBuildStamp()
{
  char* base = SDL_GetBasePath();
  if (!base)
    return "unknown";
  const std::filesystem::path folder(base);
  SDL_free(base);

  uint64_t hash = 14695981039346656037ull; // FNV-1a
  bool found = false;
  std::vector<char> chunk(1 << 16);
  for (const char* name : {"libengine.dll",
                           "libengine-core.dll",
                           "libengine-webgpu.dll",
                           "libengine.so",
                           "libengine-core.so",
                           "libengine-webgpu.so"})
  {
    std::ifstream in(folder / name, std::ios::binary);
    if (!in)
      continue;
    found = true;
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) ||
           in.gcount() > 0)
      for (std::streamsize i = 0; i < in.gcount(); ++i)
        hash = (hash ^ static_cast<unsigned char>(chunk[i])) * 1099511628211ull;
  }
  if (!found)
  {
    LOG_INFO("Can't find the engine libraries (looked in " + folder.string() +
             "), so a game module built for another engine won't be noticed");
    return "unknown";
  }
  return std::to_string(hash);
}

} // namespace

void EditorApp::stampModule()
{
  const std::filesystem::path module = m_builder.modulePath(*m_context.project);
  std::ofstream(stampPath(module)) << m_engineStamp.get();
  std::ofstream(scriptsStampPath(module)) << m_buildScriptsStamp;
}

void EditorApp::loadExistingModule()
{
  const std::filesystem::path module = m_builder.modulePath(*m_context.project);
  if (!std::filesystem::exists(module))
  {
    destroyScenes();
    sfs::scriptableObjects()
        .clear(); // objects of the module's types go before its code does
    m_module.unload();
    m_awaitingModule = true;
    createScenes();
    LOG_INFO("No game module built yet: press Play or Build (Ctrl+B)");
    return;
  }

  // A module built for another build of the engine may not match its types
  // (a crash, not an error): build it afresh instead of loading it. The scene
  // opens meanwhile, keeping the module's meshes and components by name until
  // the build swaps it in. (A stamp rather than file times: those don't
  // compare reliably across a WSL share.)
  std::string builtFor;
  std::ifstream(stampPath(module)) >> builtFor;
  const std::string engine = m_engineStamp.get();
  if (engine != "unknown" && builtFor != engine)
  {
    destroyScenes();
    sfs::scriptableObjects()
        .clear(); // objects of the module's types go before its code does
    m_module.unload();
    m_awaitingModule = true;
    createScenes();
    LOG_INFO("The game module was built for an older engine: rebuilding it "
             "(its meshes show once it's built)");
    requestBuild(false);
    return;
  }

  // Drop anything that used the previous project's module.
  destroyScenes();
  m_awaitingModule = !m_module.load(
      module, m_context.project->moduleName(), sfs::ModuleHost::Load::FromCopy);
  createScenes();
}

void EditorApp::play()
{
  activate(sceneWorkspace()); // the scene plays, not a prefab
  m_editSnapshot = m_document->snapshot();
  // The game sees through its camera entity, if it has one (Stop restores the
  // editor's view with the snapshot).
  m_context.render->setFollowSceneCamera(true);
  // Scriptable objects load into the playing scene's meshes, fresh each Play.
  sfs::scriptableObjects().setMeshes(&m_context.scene->renderSystem().meshes());
  sfs::startScripts(*m_context.scene,
                    m_context.scene->gameSystems(),
                    &m_context.scene->renderSystem().meshes());
  m_mode = Mode::Playing;
  m_context.playing = true;
  m_context.focusViewport = true; // the game has the keyboard straight away
  m_history.setEnabled(false);    // play-mode changes are discarded on Stop
  LOG_INFO("Playing");
  updateWindowTitle();
}

void EditorApp::onEvent(const SDL_Event& event)
{
  if (event.type != SDL_DROPFILE)
    return;
  // SDL hands over the path string; it must be freed.
  const std::unique_ptr<char, decltype(&SDL_free)> file(
      event.drop.file, SDL_free);
  if (!m_assets || !file)
    return;
  const std::filesystem::path folder =
      m_assetPanel ? m_assetPanel->importFolder() : std::filesystem::path();
  if (const std::optional<sfs::AssetId> id =
          m_assets->import(file.get(), folder))
  {
    LOG_INFO("Imported " + m_assets->find(*id)->path.generic_string());
    m_context.selectedAsset = *id;
    m_context.selection = {};
  }
}

sfs::Game::SceneView EditorApp::sceneView() const
{
  const glm::ivec4& rect = m_context.viewportRect;
  return SceneView{
      m_context.viewportWindowId, SDL_Rect{rect.x, rect.y, rect.z, rect.w}};
}

void EditorApp::onQuit()
{
  if (m_mode == Mode::Playing)
  {
    LOG_INFO("The game quit");
    stop();
  }
}

void EditorApp::onLoadScene(const std::filesystem::path& file)
{
  if (m_mode != Mode::Playing || !m_context.project)
    return;

  // As in an exported game. The scene being edited is untouched: it isn't
  // the document's file that's replaced, and Stop restores it.
  if (playScene(file))
    LOG_INFO("Playing " + file.generic_string());
  else
    stop();
}

std::filesystem::path EditorApp::gameFolder() const
{
  return m_context.project ? m_context.project->root()
                           : std::filesystem::current_path();
}

sfs::Scene3D& EditorApp::replaceScene()
{
  replaceMainScene();
  m_context.render->setFollowSceneCamera(
      true); // only played into, never edited
  return *m_context.scene;
}

void EditorApp::stop()
{
  if (m_mode != Mode::Playing)
    return;

  // Everything that happened in Play is discarded, as in other engines.
  replaceMainScene();
  sfs::scriptableObjects().setMeshes(
      nullptr); // what Play loaded, gone with its scene
  m_document->restore(m_editSnapshot);
  m_mode = Mode::Edit;
  m_context.playing = false;
  m_history.setEnabled(true);
  LOG_INFO("Stopped");
  updateWindowTitle();
}

void EditorApp::drawExportDialog()
{
  if (!ImGui::BeginPopupModal(
          kExportDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  if (!m_context.project)
  {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const Project& project = *m_context.project;

  ImGui::TextUnformatted(
      "Builds the scripts, then writes a playable copy of the game.");
  ImGui::Spacing();

  ImGui::SetNextItemWidth(320.0f);
  if (ImGui::BeginCombo(
          "Start scene",
          m_exportStartScene.empty() ? "(choose)" : m_exportStartScene.c_str()))
  {
    for (const std::filesystem::path& scene : sceneFiles(project))
    {
      const std::string relative =
          (std::filesystem::path("scenes") / scene.filename()).generic_string();
      if (ImGui::Selectable(relative.c_str(), relative == m_exportStartScene))
        m_exportStartScene = relative;
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(320.0f);
  widgets::inputText("##folder", m_exportFolder);
  ImGui::SameLine();
  if (ImGui::Button("Browse..."))
    if (const auto folder = fileDialog::pickFolder(
            window, "Export the game into", m_exportFolder))
      m_exportFolder = folder->string();
  ImGui::SameLine();
  ImGui::TextUnformatted("Folder");
  // The game gets a folder of its own inside the chosen one.
  ImGui::TextDisabled("Creates %s", exportTarget().string().c_str());
  ImGui::SetNextItemWidth(320.0f);
  ImGui::InputInt2("Window size", m_exportWindowSize);
  m_exportWindowSize[0] = std::clamp(m_exportWindowSize[0], 320, 7680);
  m_exportWindowSize[1] = std::clamp(m_exportWindowSize[1], 240, 4320);

  ImGui::Spacing();
  ImGui::BeginDisabled(m_exportStartScene.empty() || m_exportFolder.empty());
  if (ImGui::Button("Export"))
  {
    // Everything as it's being edited goes into the game: open prefabs'
    // changes applied, then the scene saved.
    if (m_mode == Mode::Edit)
    {
      for (const auto& workspace : m_workspaces)
        if (workspace->prefab.valid() &&
            workspace->history->revision() != workspace->appliedRevision)
          applyPrefab(*workspace);
      if (m_document->hasPath())
        m_document->save();
    }
    m_context.project->setStartScene(m_exportStartScene);
    m_lastExportFolder = m_exportFolder;
    requestBuild(false);
    m_exportAfterBuild = moduleBuilding();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel"))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void EditorApp::exportGame()
{
  if (!m_context.project)
    return;

  const char* base = SDL_GetBasePath();
  const GameExporter exporter(base ? std::filesystem::path(base)
                                   : std::filesystem::current_path());

  GameExporter::Options options;
  options.output = exportTarget();
  options.startScene = m_context.project->startScene();
  options.module = m_builder.modulePath(*m_context.project);
  options.windowWidth = m_exportWindowSize[0];
  options.windowHeight = m_exportWindowSize[1];

  if (!exporter.run(*m_context.project, options).empty())
    shell::openExternally(options.output);
}

std::filesystem::path EditorApp::exportTarget() const
{
  const std::string name =
      m_context.project ? m_context.project->name() : std::string("Game");
  const std::filesystem::path executable = GameExporter::executableName(name);
  return std::filesystem::path(m_exportFolder) / executable.stem();
}
