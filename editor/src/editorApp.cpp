#include "editorApp.h"

#include "panels/assetBrowserPanel.h"
#include "panels/codePanel.h"
#include "panels/consolePanel.h"
#include "panels/hierarchyPanel.h"
#include "panels/inspectorPanel.h"
#include "panels/sceneSettingsPanel.h"
#include "panels/viewportPanel.h"
#include "platform/fileDialog.h"
#include "platform/shell.h"
#include "scene/editorScene.h"
#include "widgets/widgets.h"

#include "engine/core/components/camera3DComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "engine/webgpu/webGpuRenderBackend.h"

#include "engine/core/logger/logger.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder (default layout)
#include "imguizmo/ImGuizmo.h"

#include <SDL_filesystem.h>
#include <SDL_video.h>

#include <algorithm>

namespace
{
constexpr const char* kOpenDialog = "Open Project";
constexpr const char* kNewDialog = "New Project";
constexpr const char* kSaveSceneDialog = "Save Scene As";
constexpr const char* kNewScriptDialog = "New Script";
constexpr const char* kExportDialog = "Export Game";

// The project's scene files, sorted by name.
std::vector<std::filesystem::path> sceneFiles(const Project& project)
{
  std::vector<std::filesystem::path> files;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(project.scenesFolder(), error))
    if (entry.is_regular_file() && entry.path().extension() == SceneDocument::kExtension)
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  return files;
}
} // namespace

EditorApp::EditorApp(std::filesystem::path projectFolder)
    : m_startupProject(std::move(projectFolder))
{
}

std::unique_ptr<sfs::IRenderBackend> EditorApp::makeRenderBackend()
{
  return std::make_unique<sfs::WebGpuRenderBackend>();
}

void EditorApp::onSetup()
{
  // The editor draws its own UI; the engine's debug overlay would duplicate it.
  setDebugUiVisible(false);
  m_context.history = &m_history;

  auto& backend = static_cast<sfs::WebGpuRenderBackend&>(*renderBackend());

  m_panels.push_back(std::make_unique<ConsolePanel>()); // first, to catch setup logs
  m_panels.push_back(std::make_unique<ViewportPanel>(backend));
  m_panels.push_back(std::make_unique<HierarchyPanel>());
  m_panels.push_back(std::make_unique<InspectorPanel>());
  m_panels.push_back(std::make_unique<SceneSettingsPanel>());
  auto codePanel = std::make_unique<CodePanel>(m_environment);
  m_codePanel = codePanel.get();
  m_panels.push_back(std::move(codePanel));
  m_panels.push_back(std::make_unique<AssetBrowserPanel>(AssetBrowserPanel::Actions{
      .openScene = [this](const std::filesystem::path& path) { openScene(path); },
      .openText = [this](const std::filesystem::path& path) { m_codePanel->openFile(path); },
      .newScript =
          [this](scriptTemplates::Kind kind)
      {
        m_dialogScriptKind = kind;
        m_dialogScriptName.clear();
        m_requestedDialog = Dialog::NewScript;
      }}));

  createScene();

  if (!m_startupProject.empty())
    openProject(m_startupProject);
  updateWindowTitle();
}

void EditorApp::onDestroy()
{
  // Panels hold the backend's scene target and a logger sink; release them
  // while both still exist.
  m_panels.clear();
}

void EditorApp::onUpdate(double)
{
  // Build results are acted on here, at the start of a frame, never while the
  // UI is mid-draw: a module swap replaces the scene.
  if (const std::optional<bool> built = m_builder.poll())
  {
    const bool startPlaying = *built && m_playAfterBuild;
    const bool startExport = *built && m_exportAfterBuild;
    m_playAfterBuild = false;
    m_exportAfterBuild = false;
    if (*built && swapModule() && startPlaying)
      play();
    if (startExport)
      exportGame();
  }

  // The viewport sets it again while it draws, if it still has focus.
  m_context.gameHasInput = false;

  for (const auto& panel : m_panels)
    panel->update(m_context);
}

void EditorApp::onRender()
{
  sfs::IRenderBackend& backend = *renderBackend();
  backend.imguiNewFrame();
  ImGuizmo::BeginFrame();

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

  for (const auto& panel : m_panels)
    panel->draw(m_context);

  backend.imguiRenderDrawData();
}

void EditorApp::buildDefaultLayout(unsigned int dockspaceId)
{
  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

  ImGuiID center = dockspaceId;
  const ImGuiID left =
      ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
  const ImGuiID right =
      ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
  const ImGuiID bottom =
      ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
  ImGuiID rightBottom = 0;
  const ImGuiID rightTop =
      ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.6f, nullptr, &rightBottom);

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
    if (ImGui::MenuItem("Export Game...", nullptr, false,
                        m_context.project && !m_builder.busy()))
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

  if (m_context.project)
  {
    const std::string label = m_context.project->root().string();
    ImGui::SameLine(ImGui::GetWindowWidth() -
                    ImGui::CalcTextSize(label.c_str()).x - 16.0f);
    ImGui::TextDisabled("%s", label.c_str());
  }

  ImGui::EndMainMenuBar();
}

void EditorApp::drawSceneMenu()
{
  // Scenes live in the project's scenes folder, so they need a project.
  const bool haveProject = m_context.project.has_value();

  if (ImGui::MenuItem("New Scene", nullptr, false, m_mode == Mode::Edit))
  {
    m_document->newScene();
    m_history.clear();
    m_context.selection = {};
    updateWindowTitle();
  }

  if (ImGui::BeginMenu("Open Scene", haveProject))
  {
    const std::vector<std::filesystem::path> files = sceneFiles(*m_context.project);
    for (const std::filesystem::path& file : files)
      if (ImGui::MenuItem(file.stem().string().c_str(), nullptr, file == m_document->path()))
        openScene(file);
    if (files.empty())
      ImGui::TextDisabled("No scenes in %s", m_context.project->scenesFolder().string().c_str());
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
    m_history.undo();
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, kGlobal) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, kGlobal))
    m_history.redo();
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
  if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_history.canUndo()))
    m_history.undo();
  if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_history.canRedo()))
    m_history.redo();
  ImGui::Separator();
  if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_context.hasSelection()))
    duplicateSelection();
  if (ImGui::MenuItem("Delete", "Del", false, m_context.hasSelection()))
    deleteSelection();
  ImGui::Separator();
  const bool canAlign = m_mode == Mode::Edit && m_context.hasSelection() &&
                        m_context.selection.hasComponent<sfs::Transform3DComponent>();
  if (ImGui::MenuItem("Align With View", "Ctrl+Shift+F", false, canAlign))
    alignSelectionWithView();
  ImGui::SetItemTooltip("Move the selection to where the viewport is looking from");
  if (ImGui::MenuItem("Align View With Selected", nullptr, false, canAlign))
    alignViewWithSelection();
  ImGui::SetItemTooltip("Look through the selection (a camera's view, while editing)");
  ImGui::EndMenu();
}

void EditorApp::duplicateSelection()
{
  if (!m_context.hasSelection())
    return;

  m_history.change(
      [&]
      {
        const sfs::SceneSerializer serializer(sfs::reflect::types(),
                                              &m_context.render->meshes());
        // The whole branch, beside the original (same parent).
        sfs::Entity copy = serializer.loadEntity(
            serializer.saveEntity(*m_context.scene, m_context.selection),
            *m_context.scene,
            sfs::hierarchy::parentOf(m_context.selection));
        if (copy.hasComponent<sfs::NameComponent>())
          copy.getComponent<sfs::NameComponent>().name += " (copy)";
        m_context.scene->flushEntities();
        m_context.selection = copy;
      });
}

void EditorApp::deleteSelection()
{
  if (!m_context.hasSelection())
    return;

  // Cleared inside the change, so undo brings the entity back selected.
  m_history.change(
      [&]
      {
        const sfs::Entity doomed = m_context.selection;
        m_context.selection = {};
        m_context.scene->destroyBranch(doomed);
      });
}

void EditorApp::alignSelectionWithView()
{
  if (m_mode != Mode::Edit || !m_context.hasSelection() ||
      !m_context.selection.hasComponent<sfs::Transform3DComponent>())
    return;

  m_history.change(
      [&]
      {
        const sfs::Camera3D& view = m_context.render->camera();
        sfs::Entity entity = m_context.selection;
        const glm::vec3 scale = entity.getComponent<sfs::Transform3DComponent>().scale;
        sfs::hierarchy::setWorldMatrix(
            entity, sfs::Transform3DComponent(view.position(), view.orientation()).matrix());
        entity.getComponent<sfs::Transform3DComponent>().scale = scale;
        if (entity.hasComponent<sfs::Camera3DComponent>())
          entity.getComponent<sfs::Camera3DComponent>().focusDistance = view.distance;
      });
}

void EditorApp::alignViewWithSelection()
{
  if (!m_context.hasSelection() || !m_context.selection.hasComponent<sfs::Transform3DComponent>())
    return;

  // Not an edit: the editor's view isn't part of the scene's undo history.
  const sfs::Entity entity = m_context.selection;
  const glm::mat4 world = sfs::hierarchy::worldMatrix(entity);
  sfs::Camera3D& view = m_context.render->camera();
  if (entity.hasComponent<sfs::Camera3DComponent>())
    entity.getComponent<sfs::Camera3DComponent>().applyTo(view, world);
  else
    view.setPose(glm::vec3(world[3]), glm::vec3(world[0]), view.distance);
}

void EditorApp::drawScriptsMenu()
{
  using scriptTemplates::Kind;
  if (!ImGui::BeginMenu("Scripts", m_context.project.has_value()))
    return;

  for (const Kind kind : {Kind::GameObject, Kind::System, Kind::Component})
  {
    const std::string label = std::string("New ") + scriptTemplates::label(kind) + "...";
    if (ImGui::MenuItem(label.c_str()))
    {
      m_dialogScriptKind = kind;
      m_dialogScriptName.clear();
      m_requestedDialog = Dialog::NewScript;
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Build", "Ctrl+B", false, !m_builder.busy()))
    requestBuild(false);
  ImGui::EndMenu();
}

void EditorApp::drawPlayControls()
{
  // Centred in the menu bar, like most engines.
  const char* label = m_builder.busy() ? "Building..."
                      : m_mode == Mode::Playing ? "Stop"
                                                : "Play";
  const float width = ImGui::CalcTextSize("Building...").x + 24.0f;
  ImGui::SameLine((ImGui::GetWindowWidth() - width) * 0.5f);

  ImGui::BeginDisabled(m_builder.busy() || !m_context.project);
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
    m_exportFolder = (project.exportFolder() / m_environment.platformName()).string();
    if (!m_lastExportFolder.empty())
      m_exportFolder = m_lastExportFolder;
    m_exportStartScene = project.startScene().generic_string();
    if (m_exportStartScene.empty() && !m_document->path().empty())
      m_exportStartScene =
          (std::filesystem::path("scenes") / m_document->path().filename()).generic_string();
    ImGui::OpenPopup(kExportDialog);
  }
  m_requestedDialog = Dialog::None;

  if (ImGui::BeginPopupModal(kNewScriptDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("New %s (class name, also the file name):",
                scriptTemplates::label(m_dialogScriptKind));
    ImGui::SetNextItemWidth(320.0f);
    widgets::inputText("##scriptName", m_dialogScriptName);
    const bool valid = scriptTemplates::isValidName(m_dialogScriptName);
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Create") && m_context.project)
    {
      if (const auto created = scriptTemplates::create(m_dialogScriptKind,
                                                       m_dialogScriptName,
                                                       m_context.project->scriptsFolder()))
        m_codePanel->openFile(*created);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  drawExportDialog();

  if (ImGui::BeginPopupModal(kSaveSceneDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::TextUnformatted("Scene name (saved in the project's scenes folder):");
    ImGui::SetNextItemWidth(320.0f);
    widgets::inputText("##sceneName", m_dialogSceneName);
    if (ImGui::Button("Save") && !m_dialogSceneName.empty() && m_context.project)
    {
      std::filesystem::path path = m_context.project->scenesFolder() / m_dialogSceneName;
      path.replace_extension(SceneDocument::kExtension);
      m_document->saveAs(path);
      updateWindowTitle();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopupModal(kOpenDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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

  if (ImGui::BeginPopupModal(kNewDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::SetNextItemWidth(480.0f);
    widgets::inputText("Name", m_dialogName);
    ImGui::SetNextItemWidth(480.0f);
    widgets::inputText("Folder", m_dialogPath);
    if (ImGui::Button("Create") && !m_dialogName.empty() && !m_dialogPath.empty())
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
  if (std::optional<Project> project = Project::open(folder))
  {
    m_context.project = std::move(project);
    projectOpened();
  }
}

void EditorApp::createProject(const std::filesystem::path& folder,
                              const std::string& name)
{
  if (std::optional<Project> project = Project::create(folder, name))
  {
    m_context.project = std::move(project);
    projectOpened();
  }
}

void EditorApp::projectOpened()
{
  if (m_mode == Mode::Playing)
    stop();
  m_context.project->ensureBuildFiles();

  // Load the last built module first, so scenes using its components load.
  loadExistingModule();

  // Start on the project's main scene, else its first, else a new one.
  const std::vector<std::filesystem::path> files = sceneFiles(*m_context.project);
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
  m_document->open(path);
  m_history.clear();
  m_context.selection = {};
  updateWindowTitle();
}

void EditorApp::saveScene()
{
  if (!m_context.project || m_mode != Mode::Edit)
    return;
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
    const std::string scene =
        m_document->hasPath() ? m_document->path().stem().string() : "Untitled";
    title = m_context.project->name() + " - " + scene +
            (m_mode == Mode::Playing ? " [Playing]" : "") + " - SfsEditor";
  }
  SDL_SetWindowTitle(window, title.c_str());
}

void EditorApp::requestBuild(bool thenPlay)
{
  if (!m_context.project || m_builder.busy())
    return;
  if (m_mode == Mode::Playing)
    stop(); // rebuilding replaces the module the play session runs
  if (m_builder.start(*m_context.project))
    m_playAfterBuild = thenPlay;
}

void EditorApp::destroyScene()
{
  m_context.selection = {};
  m_context.scene = nullptr;
  m_context.render = nullptr;
  sceneManager.destroyAll();
}

void EditorApp::createScene()
{
  m_context.scene = sceneManager.createScene<EditorScene>();
  m_context.render = &m_context.scene->renderSystem();
  // While editing, the viewport is the editor's own free view.
  m_context.render->setFollowSceneCamera(false);
  if (m_document)
    m_document->rebind(*m_context.scene);
  else
    m_document.emplace(*m_context.scene);
  m_context.document = &*m_document;
}

bool EditorApp::swapModule()
{
  const std::filesystem::path module = m_builder.modulePath(*m_context.project);
  const nlohmann::json snapshot = m_document->snapshot();

  // The scene can't outlive the code behind its components: tear it down,
  // swap the module, then rebuild it from the snapshot.
  destroyScene();
  m_module.unload();
  const bool loaded = m_module.load(module, m_context.project->moduleName(),
                                    sfs::ModuleHost::Load::FromCopy);
  createScene();
  m_document->restore(snapshot);
  return loaded;
}

void EditorApp::loadExistingModule()
{
  const std::filesystem::path module = m_builder.modulePath(*m_context.project);
  if (!std::filesystem::exists(module))
  {
    destroyScene();
    m_module.unload();
    createScene();
    LOG_INFO("No game module built yet: press Play or Build (Ctrl+B)");
    return;
  }

  // Drop anything that used the previous project's module.
  destroyScene();
  m_module.load(module, m_context.project->moduleName(), sfs::ModuleHost::Load::FromCopy);
  createScene();
}

void EditorApp::play()
{
  m_editSnapshot = m_document->snapshot();
  // The game sees through its camera entity, if it has one (Stop restores the
  // editor's view with the snapshot).
  m_context.render->setFollowSceneCamera(true);
  sfs::startScripts(*m_context.scene, m_context.scene->gameSystems(),
                    &m_context.scene->renderSystem().meshes());
  m_mode = Mode::Playing;
  m_context.playing = true;
  m_history.setEnabled(false); // play-mode changes are discarded on Stop
  LOG_INFO("Playing");
  updateWindowTitle();
}

sfs::Game::SceneView EditorApp::sceneView() const
{
  const glm::ivec4& rect = m_context.viewportRect;
  return SceneView{m_context.viewportWindowId, SDL_Rect{rect.x, rect.y, rect.z, rect.w}};
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
  return m_context.project ? m_context.project->root() : std::filesystem::current_path();
}

sfs::Scene3D& EditorApp::replaceScene()
{
  destroyScene();
  createScene();
  m_context.render->setFollowSceneCamera(true); // only played into, never edited
  return *m_context.scene;
}

void EditorApp::stop()
{
  if (m_mode != Mode::Playing)
    return;

  // Everything that happened in Play is discarded, as in other engines.
  destroyScene();
  createScene();
  m_document->restore(m_editSnapshot);
  m_mode = Mode::Edit;
  m_context.playing = false;
  m_history.setEnabled(true);
  LOG_INFO("Stopped");
  updateWindowTitle();
}

void EditorApp::drawExportDialog()
{
  if (!ImGui::BeginPopupModal(kExportDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  if (!m_context.project)
  {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const Project& project = *m_context.project;

  ImGui::TextUnformatted("Builds the scripts, then writes a playable copy of the game.");
  ImGui::Spacing();

  ImGui::SetNextItemWidth(320.0f);
  if (ImGui::BeginCombo("Start scene", m_exportStartScene.empty() ? "(choose)"
                                                                  : m_exportStartScene.c_str()))
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
    if (const auto folder = fileDialog::pickFolder(window, "Export the game into", m_exportFolder))
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
    m_context.project->setStartScene(m_exportStartScene);
    m_lastExportFolder = m_exportFolder;
    requestBuild(false);
    m_exportAfterBuild = m_builder.busy();
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
  const GameExporter exporter(base ? std::filesystem::path(base) : std::filesystem::current_path());

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
  const std::string name = m_context.project ? m_context.project->name() : std::string("Game");
  const std::filesystem::path executable = GameExporter::executableName(name);
  return std::filesystem::path(m_exportFolder) / executable.stem();
}
