#include "panels/assetBrowserPanel.h"

#include "assets/assetDrop.h"
#include "assets/materialFiles.h"
#include "editorContext.h"
#include "platform/shell.h"
#include "scene/sceneDocument.h"
#include "widgets/widgets.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/logger/logger.h"

#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
constexpr auto kRefreshInterval = std::chrono::seconds(2);

// The project folders shown, in this order.
constexpr const char* kFolders[] = {"scenes", "scripts", "assets"};

// Drag and drop payload: an absolute path under assets/, NUL-terminated.
using assetDrop::kAssetPayload;

// Files the Code panel edits: sources, build scripts, plain text.
bool isTextFile(const fs::path& path)
{
  static const std::vector<std::string> kExtensions = {
      ".h", ".hpp", ".hh", ".inl", ".cpp", ".cc", ".cxx", ".c",
      ".cmake", ".txt", ".md", ".lua", ".json", ".glsl", ".wgsl"};
  const std::string extension = path.extension().string();
  return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

bool isWithin(const fs::path& path, const fs::path& folder)
{
  const std::string p = path.lexically_normal().generic_string();
  const std::string f = folder.lexically_normal().generic_string();
  return p == f || p.starts_with(f + "/");
}

std::string label(const fs::path& path) { return path.filename().string(); }
} // namespace

// --- helpers ------------------------------------------------------------------------

bool AssetBrowserPanel::inAssets(const fs::path& path) const
{
  return !m_assets.empty() && isWithin(path, m_assets);
}

fs::path AssetBrowserPanel::assetRelative(const fs::path& path) const
{
  const fs::path relative = path.lexically_relative(m_assets);
  return relative == "." ? fs::path() : relative;
}

fs::path AssetBrowserPanel::importFolder() const
{
  return inAssets(m_open) ? assetRelative(m_open) : fs::path();
}

void AssetBrowserPanel::refresh(EditorContext& context)
{
  m_root = context.project->root();
  m_assets = context.project->assetsFolder();
  if (m_open.empty() || !fs::is_directory(m_open))
    m_open = m_assets;
  if (context.assets)
    context.assets->refresh();
  m_listings.clear(); // re-read scenes/ and scripts/ on next use
  m_lastRefresh = std::chrono::steady_clock::now();
}

std::vector<AssetBrowserPanel::Entry> AssetBrowserPanel::listFolder(const EditorContext& context,
                                                                    const fs::path& folder)
{
  // assets/: from the catalogue (no .meta files, kept current by its edits).
  if (inAssets(folder) && context.assets)
  {
    const fs::path relative = assetRelative(folder);
    std::vector<Entry> entries;
    for (const fs::path& sub : context.assets->folders())
      if (sub.parent_path() == relative)
        entries.push_back({m_assets / sub, true});
    for (const sfs::AssetRecord& record : context.assets->records())
      if (record.path.parent_path() == relative)
        entries.push_back({m_assets / record.path, false});
    return entries;
  }

  // scenes/, scripts/: the folder itself, read at most once per refresh.
  const auto cached = m_listings.find(folder);
  if (cached != m_listings.end())
    return cached->second;

  std::vector<Entry> entries;
  std::error_code error;
  for (const auto& item : fs::directory_iterator(folder, error))
    if (!label(item.path()).starts_with("."))
      entries.push_back({item.path(), item.is_directory(error)});
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
            { return a.directory != b.directory ? a.directory : a.path.filename() < b.path.filename(); });
  m_listings.emplace(folder, entries);
  return entries;
}

// --- drawing ------------------------------------------------------------------------

void AssetBrowserPanel::drawContents(EditorContext& context)
{
  if (!context.project)
  {
    ImGui::TextDisabled("No project open");
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_root != context.project->root() || now - m_lastRefresh > kRefreshInterval)
    refresh(context);

  drawToolbar(context);
  ImGui::Separator();

  // Folder tree | contents, side by side.
  const float treeWidth = std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.25f);
  ImGui::BeginChild("Folders", ImVec2(treeWidth, 0.0f), ImGuiChildFlags_ResizeX);
  for (const char* folder : kFolders)
    if (fs::is_directory(m_root / folder))
      drawFolderTree(context, m_root / folder);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("Contents");
  drawContentsPane(context);
  ImGui::EndChild();

  drawDeleteConfirmation(context);
}

void AssetBrowserPanel::drawToolbar(EditorContext& context)
{
  if (ImGui::SmallButton("Refresh"))
    refresh(context);
  ImGui::SameLine();
  if (ImGui::SmallButton("Show in Folder"))
    shell::openExternally(m_open);
  ImGui::SameLine();

  // Breadcrumbs to the open folder, each one a way back up.
  const fs::path relative = m_open.lexically_relative(m_root);
  fs::path walked = m_root;
  bool first = true;
  for (const fs::path& part : relative)
  {
    walked /= part;
    if (!first)
    {
      ImGui::SameLine(0.0f, 2.0f);
      ImGui::TextDisabled("/");
      ImGui::SameLine(0.0f, 2.0f);
    }
    first = false;
    ImGui::PushID(walked.generic_string().c_str());
    if (ImGui::SmallButton(part.string().c_str()))
      m_open = walked;
    dropTarget(context, walked);
    ImGui::PopID();
  }
  if (inAssets(m_open))
  {
    ImGui::SameLine();
    ImGui::TextDisabled("  (drop files on the window to import here)");
  }
}

void AssetBrowserPanel::drawFolderTree(EditorContext& context, const fs::path& folder)
{
  std::vector<Entry> subfolders;
  for (const Entry& entry : listFolder(context, folder))
    if (entry.directory)
      subfolders.push_back(entry);

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                             ImGuiTreeNodeFlags_SpanAvailWidth;
  if (subfolders.empty())
    flags |= ImGuiTreeNodeFlags_Leaf;
  if (folder == m_open)
    flags |= ImGuiTreeNodeFlags_Selected;
  if (isWithin(m_open, folder) && folder != m_open)
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);

  const bool open = ImGui::TreeNodeEx(folder.generic_string().c_str(), flags, "%s", label(folder).c_str());
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    m_open = folder;
  if (folder != m_assets)
    dragSource(context, {folder, true});
  dropTarget(context, folder);
  if (ImGui::BeginPopupContextItem())
  {
    drawFolderMenu(context, folder);
    ImGui::EndPopup();
  }

  if (open)
  {
    for (const Entry& sub : subfolders)
      drawFolderTree(context, sub.path);
    ImGui::TreePop();
  }
}

void AssetBrowserPanel::drawContentsPane(EditorContext& context)
{
  const std::vector<Entry> entries = listFolder(context, m_open);

  if (ImGui::BeginTable("Entries", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
  {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 4.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    for (const Entry& entry : entries)
      drawEntry(context, entry);
    ImGui::EndTable();
  }
  if (entries.empty())
    ImGui::TextDisabled("(empty)");

  // Right-click on the space below the list: this folder's menu.
  if (ImGui::BeginPopupContextWindow("FolderBackground",
                                     ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
  {
    drawFolderMenu(context, m_open);
    ImGui::EndPopup();
  }

  // F2 renames the selected asset.
  if (ImGui::IsWindowFocused() && !m_renaming && context.assets && context.selectedAsset.valid() &&
      ImGui::IsKeyPressed(ImGuiKey_F2, false))
    if (const sfs::AssetRecord* record = context.assets->find(context.selectedAsset))
      beginRename(m_assets / record->path);
}

void AssetBrowserPanel::drawEntry(EditorContext& context, const Entry& entry)
{
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::PushID(entry.path.generic_string().c_str());

  const sfs::AssetRecord* record =
      !entry.directory && inAssets(entry.path) && context.assets
          ? context.assets->findByPath(assetRelative(entry.path))
          : nullptr;

  if (m_renaming == entry.path)
  {
    drawRename(context, entry);
    ImGui::PopID();
    return;
  }

  const bool selected = record && record->id == context.selectedAsset;
  const std::string name = (entry.directory ? "[+] " : "") + label(entry.path);
  if (ImGui::Selectable(name.c_str(), selected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
  {
    if (record)
    {
      context.selectedAsset = record->id;
      context.selection = {}; // the Inspector shows the asset
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
      entry.directory ? void(m_open = entry.path) : activate(entry.path);
  }
  if (inAssets(entry.path))
    dragSource(context, entry);
  if (entry.directory)
    dropTarget(context, entry.path);

  if (ImGui::BeginPopupContextItem())
  {
    if (record)
    {
      context.selectedAsset = record->id;
      context.selection = {};
    }
    if (!entry.directory && ImGui::MenuItem("Open"))
      activate(entry.path);
    if (inAssets(entry.path))
    {
      if (ImGui::MenuItem("Rename", "F2"))
        beginRename(entry.path);
      if (ImGui::MenuItem("Delete..."))
        m_pendingDelete = entry.path;
    }
    if (ImGui::MenuItem("Show in Folder"))
      shell::openExternally(entry.directory ? entry.path : entry.path.parent_path());
    ImGui::EndPopup();
  }

  ImGui::TableNextColumn();
  if (entry.directory)
    ImGui::TextDisabled("Folder");
  else if (record)
    ImGui::TextDisabled("%s", sfs::assetKindName(record->kind));
  else
    ImGui::TextDisabled("%s", entry.path.extension().string().c_str());
  ImGui::PopID();
}

bool AssetBrowserPanel::drawRename(EditorContext& context, const Entry& entry)
{
  if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive())
    ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(-1.0f);
  const bool done = widgets::inputText("##rename", m_renameText, ImGuiInputTextFlags_EnterReturnsTrue |
                                                                     ImGuiInputTextFlags_AutoSelectAll);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!done && ImGui::IsItemDeactivated()))
  {
    m_renaming.reset();
    return false;
  }
  if (!done)
    return false;

  m_renaming.reset();
  std::string name = m_renameText;
  if (name.empty() || name.find_first_of("/\\") != std::string::npos || !context.assets)
    return false;
  // Keep the file's type if the new name leaves the extension off.
  if (!entry.directory && fs::path(name).extension().empty())
    name += entry.path.extension().string();
  if (name == label(entry.path))
    return false;

  const fs::path target = assetRelative(entry.path).parent_path() / name;
  if (entry.directory)
  {
    const bool wasOpen = isWithin(m_open, entry.path);
    if (context.assets->moveFolder(assetRelative(entry.path), target) && wasOpen)
      m_open = m_assets / target;
  }
  else if (const sfs::AssetRecord* record = context.assets->findByPath(assetRelative(entry.path)))
    context.assets->move(record->id, target);
  return true;
}

void AssetBrowserPanel::drawFolderMenu(EditorContext& context, const fs::path& folder)
{
  if (folder.filename() == "scripts")
  {
    using scriptTemplates::Kind;
    for (const Kind kind : {Kind::GameObject, Kind::System, Kind::Component})
    {
      const std::string itemLabel = std::string("New ") + scriptTemplates::label(kind) + "...";
      if (ImGui::MenuItem(itemLabel.c_str()) && m_actions.newScript)
        m_actions.newScript(kind);
    }
    ImGui::Separator();
  }
  if (inAssets(folder) && context.assets)
  {
    // A text field right in the menu: type a name, Enter creates it.
    ImGui::TextUnformatted("New Folder:");
    ImGui::SetNextItemWidth(180.0f);
    if (widgets::inputText("##newFolder", m_newFolderName, ImGuiInputTextFlags_EnterReturnsTrue) &&
        !m_newFolderName.empty() && m_newFolderName.find_first_of("/\\") == std::string::npos)
    {
      context.assets->createFolder(assetRelative(folder) / m_newFolderName);
      m_newFolderName.clear();
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("New Material"))
      if (const auto id = materialFiles::create(*context.assets, assetRelative(folder), "New Material"))
        if (const sfs::AssetRecord* record = context.assets->find(*id))
        {
          context.selectedAsset = *id;
          context.selection = {};
          beginRename(m_assets / record->path);
        }
    if (folder != m_assets)
    {
      if (ImGui::MenuItem("Rename"))
        beginRename(folder);
      if (ImGui::MenuItem("Delete..."))
        m_pendingDelete = folder;
    }
    ImGui::Separator();
  }
  if (ImGui::MenuItem("Show in Folder"))
    shell::openExternally(folder);
}

void AssetBrowserPanel::drawDeleteConfirmation(EditorContext& context)
{
  constexpr const char* kPopup = "Delete?";
  if (m_pendingDelete && !ImGui::IsPopupOpen(kPopup))
    ImGui::OpenPopup(kPopup);
  if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;

  const fs::path path = m_pendingDelete.value_or(fs::path());
  const bool folder = fs::is_directory(path);
  ImGui::Text("Delete %s%s?", label(path).c_str(), folder ? " and everything in it" : "");
  ImGui::TextDisabled("This can't be undone. Anything using it loses it.");
  ImGui::Spacing();
  if (ImGui::Button("Delete") && context.assets)
  {
    if (folder)
    {
      context.assets->removeFolder(assetRelative(path));
      if (isWithin(m_open, path))
        m_open = path.parent_path();
    }
    else if (const sfs::AssetRecord* record = context.assets->findByPath(assetRelative(path)))
    {
      if (record->id == context.selectedAsset)
        context.selectedAsset = {};
      context.assets->remove(record->id);
    }
    m_pendingDelete.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
  {
    m_pendingDelete.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// --- drag and drop ---------------------------------------------------------------------

void AssetBrowserPanel::dragSource(const EditorContext&, const Entry& entry)
{
  if (!inAssets(entry.path) || entry.path == m_assets || !ImGui::BeginDragDropSource())
    return;
  const std::string path = entry.path.generic_string();
  ImGui::SetDragDropPayload(kAssetPayload, path.c_str(), path.size() + 1);
  ImGui::TextUnformatted(label(entry.path).c_str());
  ImGui::EndDragDropSource();
}

void AssetBrowserPanel::dropTarget(EditorContext& context, const fs::path& folder)
{
  if (!inAssets(folder) || !context.assets || !ImGui::BeginDragDropTarget())
    return;

  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  if (peek && peek->IsDataType(kAssetPayload))
  {
    const fs::path dragged(static_cast<const char*>(peek->Data));
    // Not onto itself, its own folder, or (a folder) into itself.
    const bool allowed = dragged.parent_path() != folder && !isWithin(folder, dragged);
    if (allowed && ImGui::AcceptDragDropPayload(kAssetPayload))
    {
      const fs::path target = assetRelative(folder) / dragged.filename();
      bool moved = false;
      if (fs::is_directory(dragged))
        moved = context.assets->moveFolder(assetRelative(dragged), target);
      else if (const sfs::AssetRecord* record = context.assets->findByPath(assetRelative(dragged)))
        moved = context.assets->move(record->id, target);
      // Said in the Console, so a move made by a slip of the mouse is noticed.
      if (moved)
        LOG_INFO("Moved assets/" + assetRelative(dragged).generic_string() + " to assets/" +
                 target.generic_string());
    }
  }
  ImGui::EndDragDropTarget();
}

// --- actions ---------------------------------------------------------------------------

void AssetBrowserPanel::beginRename(const fs::path& path)
{
  // The name is edited in place in the contents list, so show its folder.
  m_open = path.parent_path();
  m_renaming = path;
  m_renameText = label(path);
}

void AssetBrowserPanel::activate(const fs::path& file)
{
  if (sfs::assetKindOf(file) == sfs::AssetKind::Material)
    return; // edited in the Inspector, which selecting it shows
  if (file.extension() == SceneDocument::kExtension && m_actions.openScene)
    m_actions.openScene(file);
  else if (isTextFile(file) && m_actions.openText)
    m_actions.openText(file);
  else
    shell::openExternally(file);
}
