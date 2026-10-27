#include "panels/assetBrowserPanel.h"

#include "editorContext.h"
#include "platform/shell.h"
#include "scene/sceneDocument.h"

#include "engine/core/logger/logger.h"

#include "imgui.h"

#include <algorithm>
#include <system_error>

namespace
{
constexpr auto kRefreshInterval = std::chrono::seconds(2);

// The project folders shown, in this order.
constexpr const char* kFolders[] = {"scenes", "scripts", "assets"};

// Files the Code panel edits: sources, build scripts, plain text.
bool isTextFile(const std::filesystem::path& path)
{
  static const std::vector<std::string> kExtensions = {
      ".h", ".hpp", ".hh", ".inl", ".cpp", ".cc", ".cxx", ".c",
      ".cmake", ".txt", ".md", ".lua", ".json", ".glsl", ".wgsl"};
  const std::string extension = path.extension().string();
  return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

std::vector<std::filesystem::path> sortedEntries(const std::filesystem::path& folder)
{
  std::vector<std::filesystem::path> entries;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error))
    if (!entry.path().filename().string().starts_with("."))
      entries.push_back(entry.path());

  // Folders first, then files, each alphabetically.
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    const bool aDir = std::filesystem::is_directory(a);
    const bool bDir = std::filesystem::is_directory(b);
    return aDir != bDir ? aDir : a.filename() < b.filename();
  });
  return entries;
}
} // namespace

void AssetBrowserPanel::refresh(const std::filesystem::path& root)
{
  const std::function<Node(const std::filesystem::path&)> build =
      [&](const std::filesystem::path& path)
  {
    Node node{path, std::filesystem::is_directory(path), {}};
    if (node.directory)
      for (const std::filesystem::path& child : sortedEntries(path))
        node.children.push_back(build(child));
    return node;
  };

  m_folders.clear();
  for (const char* folder : kFolders)
    if (std::filesystem::is_directory(root / folder))
      m_folders.push_back(build(root / folder));

  m_root = root;
  m_lastRefresh = std::chrono::steady_clock::now();
}

void AssetBrowserPanel::drawContents(EditorContext& context)
{
  if (!context.project)
  {
    ImGui::TextDisabled("No project open");
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_root != context.project->root() || now - m_lastRefresh > kRefreshInterval)
    refresh(context.project->root());

  if (ImGui::SmallButton("Refresh"))
    refresh(context.project->root());
  ImGui::SameLine();
  if (ImGui::SmallButton("Show in Folder"))
    shell::openExternally(context.project->root());
  ImGui::Separator();

  for (const Node& folder : m_folders)
    drawNode(folder);
}

void AssetBrowserPanel::drawNode(const Node& node)
{
  const std::string name = node.path.filename().string();

  if (node.directory)
  {
    const bool open = ImGui::TreeNodeEx(name.c_str(),
                                        ImGuiTreeNodeFlags_SpanAvailWidth |
                                            ImGuiTreeNodeFlags_OpenOnArrow |
                                            ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::BeginPopupContextItem())
    {
      drawContextMenu(node.path);
      ImGui::EndPopup();
    }
    if (open)
    {
      for (const Node& child : node.children)
        drawNode(child);
      if (node.children.empty())
        ImGui::TextDisabled("(empty)");
      ImGui::TreePop();
    }
    return;
  }

  ImGui::TreeNodeEx(name.c_str(),
                    ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                        ImGuiTreeNodeFlags_SpanAvailWidth);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    activate(node.path);
  if (ImGui::BeginPopupContextItem())
  {
    if (ImGui::MenuItem("Open"))
      activate(node.path);
    drawContextMenu(node.path.parent_path());
    ImGui::EndPopup();
  }
}

void AssetBrowserPanel::drawContextMenu(const std::filesystem::path& folder)
{
  if (folder.filename() == "scripts")
  {
    using scriptTemplates::Kind;
    for (const Kind kind : {Kind::GameObject, Kind::System, Kind::Component})
    {
      const std::string label = std::string("New ") + scriptTemplates::label(kind) + "...";
      if (ImGui::MenuItem(label.c_str()) && m_actions.newScript)
        m_actions.newScript(kind);
    }
    ImGui::Separator();
  }
  if (ImGui::MenuItem("Show in Folder"))
    shell::openExternally(folder);
}

void AssetBrowserPanel::activate(const std::filesystem::path& file)
{
  if (file.extension() == SceneDocument::kExtension && m_actions.openScene)
    m_actions.openScene(file);
  else if (isTextFile(file) && m_actions.openText)
    m_actions.openText(file);
  else
    shell::openExternally(file);
}
