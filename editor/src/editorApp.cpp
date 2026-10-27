#include "editorApp.h"

#include "panels/consolePanel.h"
#include "panels/hierarchyPanel.h"
#include "panels/inspectorPanel.h"
#include "panels/sceneSettingsPanel.h"
#include "panels/viewportPanel.h"
#include "scene/editorScene.h"
#include "widgets/widgets.h"

#include "engine/webgpu/webGpuRenderBackend.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder (default layout)

#include <SDL_video.h>

namespace
{
constexpr const char* kOpenDialog = "Open Project";
constexpr const char* kNewDialog = "New Project";
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

  auto& backend = static_cast<sfs::WebGpuRenderBackend&>(*renderBackend());

  m_panels.push_back(std::make_unique<ConsolePanel>()); // first, to catch setup logs
  m_panels.push_back(std::make_unique<ViewportPanel>(backend));
  m_panels.push_back(std::make_unique<HierarchyPanel>());
  m_panels.push_back(std::make_unique<InspectorPanel>());
  m_panels.push_back(std::make_unique<SceneSettingsPanel>());

  m_context.scene = sceneManager.createScene<EditorScene>();
  m_context.render = &m_context.scene->renderSystem();

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
  for (const auto& panel : m_panels)
    panel->update(m_context);
}

void EditorApp::onRender()
{
  sfs::IRenderBackend& backend = *renderBackend();
  backend.imguiNewFrame();

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
  ImGui::DockBuilderDockWindow("Viewport", center);
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
    if (ImGui::MenuItem("Exit"))
      isRunning = false;
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Window"))
  {
    for (const auto& panel : m_panels)
      ImGui::MenuItem(panel->title(), nullptr, &panel->open());
    ImGui::EndMenu();
  }

  if (m_context.project)
  {
    const std::string label = m_context.project->root().string();
    ImGui::SameLine(ImGui::GetWindowWidth() -
                    ImGui::CalcTextSize(label.c_str()).x - 16.0f);
    ImGui::TextDisabled("%s", label.c_str());
  }

  ImGui::EndMainMenuBar();
}

void EditorApp::drawProjectDialogs()
{
  // Popups must open from the same ID stack they're drawn in, so the menu
  // only requests them and they open here.
  if (m_requestedDialog == Dialog::Open)
    ImGui::OpenPopup(kOpenDialog);
  else if (m_requestedDialog == Dialog::New)
    ImGui::OpenPopup(kNewDialog);
  m_requestedDialog = Dialog::None;

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
    updateWindowTitle();
  }
}

void EditorApp::createProject(const std::filesystem::path& folder,
                              const std::string& name)
{
  if (std::optional<Project> project = Project::create(folder, name))
  {
    m_context.project = std::move(project);
    updateWindowTitle();
  }
}

void EditorApp::updateWindowTitle()
{
  const std::string title =
      m_context.project ? m_context.project->name() + " - SfsEditor" : "SfsEditor";
  SDL_SetWindowTitle(window, title.c_str());
}
