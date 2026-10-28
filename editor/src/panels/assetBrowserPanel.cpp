#include "panels/assetBrowserPanel.h"

#include "assets/assetDrop.h"
#include "assets/materialFiles.h"
#include "editorContext.h"
#include "platform/shell.h"
#include "scene/editorScene.h"
#include "scene/prefabs.h"
#include "scene/sceneDocument.h"
#include "widgets/theme.h"
#include "widgets/widgets.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/logger/logger.h"

#include "imgui.h"
#include "imgui_internal.h" // ImRect, for a drop target over the whole folder

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
// The project folders shown, in this order.
constexpr const char* kFolders[] = {"scenes", "scripts", "prefabs", "assets"};

// Drag and drop payload: an absolute path under assets/, NUL-terminated.
using assetDrop::kAssetPayload;

// `text` in at most two lines of `width`: broken at a space if it can be,
// the second cut short with "..." if it's still too long.
std::array<std::string, 2> twoLines(const std::string& text, float width)
{
  const auto fits = [width](const std::string& part) { return ImGui::CalcTextSize(part.c_str()).x <= width; };
  if (fits(text))
    return {text, ""};

  // The longest first line that fits, ending at a space, '_', '-' or '.' if it
  // can.
  std::size_t cut = 1;
  while (cut < text.size() && fits(text.substr(0, cut + 1)))
    ++cut;
  const std::size_t space = text.find_last_of(" _-.", cut - 1);
  if (space != std::string::npos && space > 0 && space + 1 < text.size())
    cut = text[space] == '.' ? space : space + 1; // a '.' starts the second line

  std::string second = text.substr(cut);
  if (!fits(second))
  {
    while (!second.empty() && !fits(second + "..."))
      second.pop_back();
    second += "...";
  }
  return {text.substr(0, cut), second};
}

// A drawn picture for something without a preview: a folder, a scene, a
// script, or a page marked with the file's type.
void drawIcon(ImDrawList* draw, ImVec2 min, ImVec2 max, bool directory, const fs::path& path)
{
  const float w = max.x - min.x, h = max.y - min.y;
  const auto at = [&](float x, float y) { return ImVec2(min.x + w * x, min.y + h * y); };
  const ImU32 ink = theme::kIconInk;
  const ImU32 fill = theme::kTile;

  if (directory)
  {
    // A folder: a tab, and the body in front of it.
    draw->AddRectFilled(at(0.14f, 0.24f), at(0.48f, 0.36f), IM_COL32(140, 182, 132, 255), 3.0f);
    draw->AddRectFilled(at(0.14f, 0.30f), at(0.86f, 0.78f), IM_COL32(170, 214, 160, 255), 5.0f);
    draw->AddRectFilled(at(0.14f, 0.38f), at(0.86f, 0.78f), IM_COL32(188, 226, 178, 255), 5.0f);
    return;
  }

  const std::string extension = path.extension().string();
  if (extension == ".prefab")
  {
    // A prefab: a sage box, drawn in three faces.
    const ImVec2 top(at(0.5f, 0.18f).x, at(0.5f, 0.18f).y), left = at(0.18f, 0.34f), right = at(0.82f, 0.34f);
    const ImVec2 middle = at(0.5f, 0.5f), bottomLeft = at(0.18f, 0.66f), bottomRight = at(0.82f, 0.66f),
                 bottom = at(0.5f, 0.82f);
    draw->AddQuadFilled(top, right, middle, left, IM_COL32(188, 226, 178, 255));
    draw->AddQuadFilled(left, middle, bottom, bottomLeft, IM_COL32(150, 194, 140, 255));
    draw->AddQuadFilled(middle, right, bottomRight, bottom, IM_COL32(120, 160, 112, 255));
    return;
  }
  if (extension == SceneDocument::kExtension)
  {
    // A scene: a little landscape in a frame.
    draw->AddRectFilled(at(0.16f, 0.22f), at(0.84f, 0.78f), IM_COL32(120, 150, 175, 255), 5.0f);
    draw->AddCircleFilled(at(0.66f, 0.38f), w * 0.07f, IM_COL32(240, 215, 140, 255));
    draw->AddTriangleFilled(at(0.18f, 0.76f), at(0.42f, 0.44f), at(0.62f, 0.76f), IM_COL32(128, 168, 116, 255));
    draw->AddTriangleFilled(at(0.44f, 0.76f), at(0.62f, 0.54f), at(0.82f, 0.76f), IM_COL32(170, 214, 160, 255));
    draw->AddRect(at(0.16f, 0.22f), at(0.84f, 0.78f), ink, 5.0f, 0, 2.0f);
    return;
  }

  // A page with its corner folded, and its type on it.
  const ImVec2 corner = at(0.74f, 0.14f);
  draw->AddRectFilled(at(0.24f, 0.12f), at(0.76f, 0.88f), fill, 4.0f);
  draw->AddRect(at(0.24f, 0.12f), at(0.76f, 0.88f), ink, 4.0f, 0, 1.5f);
  draw->AddTriangleFilled(corner, at(0.76f, 0.30f), at(0.60f, 0.12f), IM_COL32(90, 96, 92, 255));
  std::string tag = extension.size() > 1 ? extension.substr(1) : "file";
  for (char& c : tag)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (tag.size() > 4)
    tag.resize(4);
  const bool code = tag == "H" || tag == "HPP" || tag == "CPP" || tag == "C" || tag == "LUA" || tag == "WGSL";
  const ImU32 tagColor = code ? IM_COL32(222, 200, 140, 255) : theme::kAccent;
  ImFont* font = ImGui::GetFont();
  const float fontSize = std::min(ImGui::GetFontSize() * 1.1f, h * 0.2f);
  const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, tag.c_str());
  draw->AddText(font, fontSize, ImVec2(min.x + (w - textSize.x) * 0.5f, at(0, 0.58f).y), tagColor, tag.c_str());
}

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

// The panel's settings, kept in imgui.ini with the layout.
AssetBrowserPanel* g_panel = nullptr;

void registerSettings(AssetBrowserPanel* panel)
{
  g_panel = panel;
  ImGuiSettingsHandler handler;
  handler.TypeName = "SfsAssets";
  handler.TypeHash = ImHashStr("SfsAssets");
  handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void*
  { return std::strcmp(name, "Panel") == 0 ? g_panel : nullptr; };
  handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
  {
    if (auto* panel = static_cast<AssetBrowserPanel*>(entry))
      panel->readSetting(line);
  };
  handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out)
  {
    if (g_panel)
      out->appendf("[%s][Panel]\n%s\n", h->TypeName, g_panel->writeSettings().c_str());
  };
  ImGui::AddSettingsHandler(&handler);
}
} // namespace

AssetBrowserPanel::AssetBrowserPanel(Actions actions, sfs::WebGpuContext& gpu)
    : Panel("Assets"), m_actions(std::move(actions)), m_thumbnails(gpu)
{
  registerSettings(this);
}

AssetBrowserPanel::~AssetBrowserPanel()
{
  if (g_panel == this)
    g_panel = nullptr;
}

void AssetBrowserPanel::readSetting(const char* line)
{
  float size = 0.0f;
  int grid = 0;
  if (std::sscanf(line, "TileSize=%f", &size) == 1)
    m_tileSize = std::clamp(size, kMinTile, kMaxTile);
  else if (std::sscanf(line, "Grid=%d", &grid) == 1)
    m_gridView = grid != 0;
}

std::string AssetBrowserPanel::writeSettings() const
{
  return "TileSize=" + std::to_string(static_cast<int>(m_tileSize)) + "\nGrid=" + (m_gridView ? "1" : "0") + "\n";
}

std::string AssetBrowserPanel::folderLabel(const fs::path& folder) const
{
  // The project's own folders (scenes, scripts, prefabs, assets) in title case.
  std::string name = label(folder);
  if (folder.parent_path() == m_root && !name.empty())
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

// --- helpers ------------------------------------------------------------------------

bool AssetBrowserPanel::inCatalogue(const fs::path& path) const
{
  if (m_root.empty())
    return false;
  for (const fs::path& folder : sfs::AssetDatabase::projectFolders())
    if (isWithin(path, m_root / folder))
      return true;
  return false;
}

fs::path AssetBrowserPanel::assetRelative(const fs::path& path) const
{
  const fs::path relative = path.lexically_relative(m_root);
  return relative == "." ? fs::path() : relative;
}

bool AssetBrowserPanel::isCatalogueTop(const fs::path& path) const
{
  for (const fs::path& folder : sfs::AssetDatabase::projectFolders())
    if (path == m_root / folder)
      return true;
  return false;
}

fs::path AssetBrowserPanel::importFolder() const
{
  return inCatalogue(m_open) ? assetRelative(m_open) : fs::path("assets");
}

void AssetBrowserPanel::showProject(EditorContext& context)
{
  // Its catalogue is freshly scanned (as it opened): only the panel's own
  // view starts again.
  m_root = context.project->root();
  m_assets = context.project->assetsFolder();
  if (m_open.empty() || !isWithin(m_open, m_root) || !fs::is_directory(m_open))
    m_open = m_assets;
  readFolders();
}

void AssetBrowserPanel::readFolders()
{
  m_listings.clear(); // re-read scenes/ and scripts/ on next use
  m_topFolders.clear();
  for (const char* folder : kFolders)
    if (fs::is_directory(m_root / folder))
      m_topFolders.push_back(m_root / folder);
  m_thumbnails.recheck(); // files may have changed too
}

void AssetBrowserPanel::refresh(EditorContext& context)
{
  m_refreshRequested = false;
  if (!context.assets || m_scan.valid())
    return; // one at a time
  // Scanned on a worker (over a WSL share it takes a while), taken in when
  // done (see drawContents).
  m_scanRoot = context.assets->root();
  m_scan = std::async(std::launch::async,
                      [root = m_scanRoot, mode = context.assets->mode(), folders = context.assets->scannedFolders()]
                      { return sfs::AssetDatabase::scan(root, mode, folders); });
}

const std::vector<AssetBrowserPanel::Entry>& AssetBrowserPanel::listFolder(const EditorContext& context,
                                                                    const fs::path& folder)
{
  // Listings are kept until the next refresh, or (assets/) until the
  // catalogue changes.
  if (context.assets && context.assets->version() != m_listingsVersion)
  {
    m_listings.clear();
    m_listingsVersion = context.assets->version();
  }
  const auto cached = m_listings.find(folder);
  if (cached != m_listings.end())
    return cached->second;
  const auto finish = [&](std::vector<Entry> entries) -> const std::vector<Entry>&
  {
    for (Entry& entry : entries)
    {
      entry.id = entry.path.generic_string();
      entry.name = label(entry.path);
    }
    return m_listings.emplace(folder, std::move(entries)).first->second;
  };

  // assets/: from the catalogue (no .meta files, kept current by its edits).
  if (inCatalogue(folder) && context.assets)
  {
    const fs::path relative = assetRelative(folder);
    std::vector<Entry> entries;
    for (const fs::path& sub : context.assets->folders())
      if (sub.parent_path() == relative)
        entries.push_back({m_root / sub, true});
    for (const sfs::AssetRecord& record : context.assets->records())
      if (record.path.parent_path() == relative)
        entries.push_back({m_root / record.path, false, record.id});
    return finish(std::move(entries));
  }

  // scenes/, scripts/: the folder itself.
  std::vector<Entry> entries;
  std::error_code error;
  for (const auto& item : fs::directory_iterator(folder, error))
    if (!label(item.path()).starts_with("."))
      entries.push_back({item.path(), item.is_directory(error)});
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
            { return a.directory != b.directory ? a.directory : a.path.filename() < b.path.filename(); });
  return finish(std::move(entries));
}

const sfs::AssetRecord* AssetBrowserPanel::recordOf(const EditorContext& context, const Entry& entry) const
{
  return entry.asset.valid() && context.assets ? context.assets->find(entry.asset) : nullptr;
}

// --- drawing ------------------------------------------------------------------------

void AssetBrowserPanel::drawContents(EditorContext& context)
{
  if (!context.project)
  {
    ImGui::TextDisabled("No project open");
    return;
  }

  if (m_root != context.project->root())
    showProject(context);
  if (m_refreshRequested)
    refresh(context);
  if (m_scan.valid() && m_scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
  {
    sfs::AssetDatabase::Scan scan = m_scan.get();
    if (context.assets && context.assets->root() == m_scanRoot)
      context.assets->apply(std::move(scan));
    readFolders();
  }

  // Asked to show an asset: open its folder (and the tree down to it), come
  // to the front and scroll to it.
  if (context.revealAsset.valid() && context.assets)
  {
    if (const sfs::AssetRecord* record = context.assets->find(context.revealAsset))
    {
      m_open = m_root / record->path.parent_path();
      m_scrollTo = record->id;
      m_revealTree = true;
      ImGui::SetWindowFocus();
    }
    context.revealAsset = {};
  }
  if (std::exchange(context.assetFilesChanged, false))
    m_thumbnails.recheck();

  m_thumbnails.beginFrame();
  drawToolbar(context);
  ImGui::Separator();

  // Folder tree | contents, side by side.
  const float treeWidth = std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.25f);
  ImGui::BeginChild("Folders", ImVec2(treeWidth, 0.0f), ImGuiChildFlags_ResizeX);
  for (const fs::path& folder : m_topFolders)
    drawFolderTree(context, folder);
  m_revealTree = false;
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
  if (ImGui::SmallButton(m_gridView ? "List" : "Grid"))
  {
    m_gridView = !m_gridView;
    ImGui::MarkIniSettingsDirty();
  }
  ImGui::SetItemTooltip(m_gridView ? "Show the folder as a list" : "Show the folder as tiles with previews");
  if (m_gridView)
  {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    // As tall as the small buttons beside it.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    if (ImGui::SliderFloat("##size", &m_tileSize, kMinTile, kMaxTile, ""))
      ImGui::MarkIniSettingsDirty(); // remembered with the layout
    ImGui::PopStyleVar();
    ImGui::SetItemTooltip("Tile size");
  }
  ImGui::SameLine(0.0f, 16.0f);

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
    if (ImGui::SmallButton(folderLabel(walked).c_str()))
      m_open = walked;
    dropTarget(context, walked);
    ImGui::PopID();
  }
  if (inCatalogue(m_open))
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
    ImGui::SetNextItemOpen(true, m_revealTree ? ImGuiCond_Always : ImGuiCond_Once);

  const bool open = ImGui::TreeNodeEx(folder.generic_string().c_str(), flags, "%s", folderLabel(folder).c_str());
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    m_open = folder;
  if (!isCatalogueTop(folder))
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

  if (m_gridView)
    drawGrid(context, entries);
  else if (ImGui::BeginTable("Entries", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
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

  // Dropping an entity anywhere in the open folder makes a prefab of it.
  if (inCatalogue(m_open))
  {
    const ImRect area(ImGui::GetWindowPos(),
                      ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                             ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    if (ImGui::BeginDragDropTargetCustom(area, ImGui::GetID("FolderDrop")))
    {
      acceptEntity(context, m_open);
      ImGui::EndDragDropTarget();
    }
  }

  // F2 renames the selected asset.
  if (ImGui::IsWindowFocused() && !m_renaming && context.assets && context.selectedAsset.valid() &&
      ImGui::IsKeyPressed(ImGuiKey_F2, false))
    if (const sfs::AssetRecord* record = context.assets->find(context.selectedAsset))
      beginRename(m_root / record->path);
}

void AssetBrowserPanel::drawEntry(EditorContext& context, const Entry& entry)
{
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::PushID(entry.id.c_str());

  const sfs::AssetRecord* record = recordOf(context, entry);

  if (m_renaming == entry.path)
  {
    drawRename(context, entry);
    ImGui::PopID();
    return;
  }

  const bool selected = record && record->id == context.selectedAsset;
  const std::string name = (entry.directory ? "[+] " : "") + entry.name;
  const bool clicked = ImGui::Selectable(
      name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
  if (entry.asset.valid() && entry.asset == m_scrollTo)
  {
    ImGui::SetScrollHereY(0.5f);
    m_scrollTo = {};
  }
  entryBehaviour(context, entry, record, clicked, clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));

  ImGui::TableNextColumn();
  if (entry.directory)
    ImGui::TextDisabled("Folder");
  else if (record)
    ImGui::TextDisabled("%s", sfs::assetKindName(record->kind));
  else
    ImGui::TextDisabled("%s", entry.path.extension().string().c_str());
  ImGui::PopID();
}

void AssetBrowserPanel::entryBehaviour(EditorContext& context,
                                       const Entry& entry,
                                       const sfs::AssetRecord* record,
                                       bool clicked,
                                       bool doubleClicked)
{
  if (clicked && record)
  {
    context.selectedAsset = record->id;
    context.selection = {}; // the Inspector shows the asset
  }
  if (doubleClicked)
    entry.directory ? void(m_open = entry.path) : activate(entry.path);
  if (inCatalogue(entry.path))
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
    if (inCatalogue(entry.path))
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
}

void AssetBrowserPanel::drawGrid(EditorContext& context, const std::vector<Entry>& entries)
{
  const ImGuiStyle& style = ImGui::GetStyle();
  const float spacing = style.ItemSpacing.x;
  const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) /
                                                   (m_tileSize + spacing)));
  for (std::size_t i = 0; i < entries.size(); ++i)
  {
    if (i % columns != 0)
      ImGui::SameLine(0.0f, spacing);
    drawTile(context, entries[i]);
  }
}

void AssetBrowserPanel::drawTile(EditorContext& context, const Entry& entry)
{
  ImGui::PushID(entry.id.c_str());
  const sfs::AssetRecord* record = recordOf(context, entry);

  // A card: the picture, square, and the name (two lines) beneath.
  const float lineHeight = ImGui::GetTextLineHeight();
  const float padding = 6.0f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const ImVec2 size(m_tileSize, m_tileSize + lineHeight * 2.0f + padding);
  const bool renaming = m_renaming == entry.path;

  // Selected on press (not release), so a double-click's last release, after
  // it has opened something, doesn't select the tile again.
  ImGui::InvisibleButton("tile", size);
  const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  if (entry.asset.valid() && entry.asset == m_scrollTo)
  {
    ImGui::SetScrollHereY(0.5f);
    m_scrollTo = {};
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  entryBehaviour(context, entry, record, clicked, doubleClicked);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const bool selected = record && record->id == context.selectedAsset;
  const ImVec2 end(at.x + size.x, at.y + size.y);
  if (selected)
  {
    draw->AddRectFilled(at, end, theme::kAccentSoft, 7.0f);
    draw->AddRect(at, end, theme::kAccent, 7.0f, 0, 1.5f);
  }
  else if (hovered)
    draw->AddRectFilled(at, end, theme::kTileHovered, 7.0f);

  const ImVec2 pictureMin(at.x + padding, at.y + padding);
  const ImVec2 pictureMax(at.x + m_tileSize - padding, at.y + m_tileSize - padding);
  const ImTextureID preview =
      record && context.scene ? m_thumbnails.get(*record, *context.assets, context.scene->assetStore(), context.scene)
                              : ImTextureID{};
  if (preview)
  {
    draw->AddRectFilled(pictureMin, pictureMax, theme::kTile, 5.0f);
    draw->AddImageRounded(preview, pictureMin, pictureMax, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 5.0f);
  }
  else
    drawIcon(draw, pictureMin, pictureMax, entry.directory, entry.path);

  // The name, centred under the picture, wrapped to two lines.
  const float textWidth = m_tileSize - padding;
  const ImVec2 textAt(at.x + padding * 0.5f, at.y + m_tileSize);
  if (renaming)
  {
    ImGui::SetCursorScreenPos(textAt);
    ImGui::PushItemWidth(textWidth);
    drawRename(context, entry);
    ImGui::PopItemWidth();
  }
  else
  {
    const auto lines = twoLines(entry.name, textWidth);
    for (int line = 0; line < 2; ++line)
    {
      const float width = ImGui::CalcTextSize(lines[line].c_str()).x;
      draw->AddText(ImVec2(textAt.x + (textWidth - width) * 0.5f, textAt.y + lineHeight * line),
                    ImGui::GetColorU32(ImGuiCol_Text, line == 0 ? 1.0f : 0.85f),
                    lines[line].c_str());
    }
    if (hovered && entry.name != lines[0] + lines[1])
      ImGui::SetItemTooltip("%s", entry.name.c_str());
  }
  ImGui::SetCursorScreenPos(at);
  ImGui::Dummy(size); // the tile's footprint, for the layout
  ImGui::PopID();
}

bool AssetBrowserPanel::drawRename(EditorContext& context, const Entry& entry)
{
  if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive())
    ImGui::SetKeyboardFocusHere();
  if (!m_gridView)
    ImGui::SetNextItemWidth(-1.0f); // the list's column; a tile sets its own
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
      m_open = m_root / target;
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
  if (inCatalogue(folder) && context.assets)
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
          beginRename(m_root / record->path);
        }
    if (!isCatalogueTop(folder))
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
  if (!inCatalogue(entry.path) || isCatalogueTop(entry.path) || !ImGui::BeginDragDropSource())
    return;
  const std::string path = entry.path.generic_string();
  ImGui::SetDragDropPayload(kAssetPayload, path.c_str(), path.size() + 1);
  ImGui::TextUnformatted(label(entry.path).c_str());
  ImGui::EndDragDropSource();
}

void AssetBrowserPanel::acceptEntity(EditorContext& context, const fs::path& folder)
{
  // An entity dragged from the Hierarchy: made into a prefab here.
  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  if (!peek || !peek->IsDataType(assetDrop::kEntityPayload) || !context.scene ||
      !ImGui::AcceptDragDropPayload(assetDrop::kEntityPayload))
    return;
  const sfs::Entity entity = context.scene->getEntity(*static_cast<const sfs::Entity::EntityId*>(peek->Data));
  if (entity.isValid() && entity.isAlive())
    if (const auto id = prefabs::create(context, entity, assetRelative(folder)))
      context.selectedAsset = {};
}

void AssetBrowserPanel::dropTarget(EditorContext& context, const fs::path& folder)
{
  if (!inCatalogue(folder) || !context.assets || !ImGui::BeginDragDropTarget())
    return;

  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  acceptEntity(context, folder);
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
        LOG_INFO("Moved " + assetRelative(dragged).generic_string() + " to " + target.generic_string());
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
  if (sfs::assetKindOf(file) == sfs::AssetKind::Prefab)
  {
    if (m_actions.openPrefab)
      m_actions.openPrefab(file);
    return;
  }
  if (file.extension() == SceneDocument::kExtension && m_actions.openScene)
    m_actions.openScene(file);
  else if (isTextFile(file) && m_actions.openText)
    m_actions.openText(file);
  else
    shell::openExternally(file);
}
