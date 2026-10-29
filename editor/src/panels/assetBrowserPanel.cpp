#include "panels/assetBrowserPanel.h"

#include "assets/assetDrop.h"
#include "assets/gltfFiles.h"
#include "assets/materialFiles.h"
#include "assets/scriptableObjectFiles.h"
#include "editorContext.h"
#include "platform/paths.h"
#include "platform/shell.h"
#include "scene/editorScene.h"
#include "scene/prefabs.h"
#include "scene/sceneDocument.h"
#include "scripting/scriptFiles.h"
#include "widgets/theme.h"
#include "widgets/widgets.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include "imgui.h"
#include "imgui_internal.h" // ImRect (a drop target over the whole folder), the focused window

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

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
  const auto fits = [width](const std::string& part)
  { return ImGui::CalcTextSize(part.c_str()).x <= width; };
  if (fits(text))
    return {text, ""};

  // The longest first line that fits, ending at a space, '_', '-' or '.' if it
  // can.
  std::size_t cut = 1;
  while (cut < text.size() && fits(text.substr(0, cut + 1)))
    ++cut;
  const std::size_t space = text.find_last_of(" _-.", cut - 1);
  if (space != std::string::npos && space > 0 && space + 1 < text.size())
    cut =
        text[space] == '.' ? space : space + 1; // a '.' starts the second line

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
void drawIcon(ImDrawList* draw,
              ImVec2 min,
              ImVec2 max,
              bool directory,
              const fs::path& path)
{
  const float w = max.x - min.x, h = max.y - min.y;
  const auto at = [&](float x, float y)
  { return ImVec2(min.x + w * x, min.y + h * y); };
  const ImU32 ink = theme::kIconInk;
  const ImU32 fill = theme::kTile;

  if (directory)
  {
    // A folder: a tab, and the body in front of it.
    draw->AddRectFilled(
        at(0.14f, 0.24f), at(0.48f, 0.36f), IM_COL32(140, 182, 132, 255), 3.0f);
    draw->AddRectFilled(
        at(0.14f, 0.30f), at(0.86f, 0.78f), IM_COL32(170, 214, 160, 255), 5.0f);
    draw->AddRectFilled(
        at(0.14f, 0.38f), at(0.86f, 0.78f), IM_COL32(188, 226, 178, 255), 5.0f);
    return;
  }

  const std::string extension = path.extension().string();
  if (extension == ".prefab")
  {
    // A prefab: a sage box, drawn in three faces.
    const ImVec2 top(at(0.5f, 0.18f).x, at(0.5f, 0.18f).y),
        left = at(0.18f, 0.34f), right = at(0.82f, 0.34f);
    const ImVec2 middle = at(0.5f, 0.5f), bottomLeft = at(0.18f, 0.66f),
                 bottomRight = at(0.82f, 0.66f), bottom = at(0.5f, 0.82f);
    draw->AddQuadFilled(top, right, middle, left, IM_COL32(188, 226, 178, 255));
    draw->AddQuadFilled(
        left, middle, bottom, bottomLeft, IM_COL32(150, 194, 140, 255));
    draw->AddQuadFilled(
        middle, right, bottomRight, bottom, IM_COL32(120, 160, 112, 255));
    return;
  }
  if (extension == SceneDocument::kExtension)
  {
    // A scene: a little landscape in a frame.
    draw->AddRectFilled(
        at(0.16f, 0.22f), at(0.84f, 0.78f), IM_COL32(120, 150, 175, 255), 5.0f);
    draw->AddCircleFilled(
        at(0.66f, 0.38f), w * 0.07f, IM_COL32(240, 215, 140, 255));
    draw->AddTriangleFilled(at(0.18f, 0.76f),
                            at(0.42f, 0.44f),
                            at(0.62f, 0.76f),
                            IM_COL32(128, 168, 116, 255));
    draw->AddTriangleFilled(at(0.44f, 0.76f),
                            at(0.62f, 0.54f),
                            at(0.82f, 0.76f),
                            IM_COL32(170, 214, 160, 255));
    draw->AddRect(at(0.16f, 0.22f), at(0.84f, 0.78f), ink, 5.0f, 0, 2.0f);
    return;
  }

  // A page with its corner folded, and its type on it.
  const ImVec2 corner = at(0.74f, 0.14f);
  draw->AddRectFilled(at(0.24f, 0.12f), at(0.76f, 0.88f), fill, 4.0f);
  draw->AddRect(at(0.24f, 0.12f), at(0.76f, 0.88f), ink, 4.0f, 0, 1.5f);
  draw->AddTriangleFilled(
      corner, at(0.76f, 0.30f), at(0.60f, 0.12f), IM_COL32(90, 96, 92, 255));
  std::string tag = extension.size() > 1 ? extension.substr(1) : "file";
  for (char& c : tag)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (tag.size() > 4)
    tag.resize(4);
  const bool code = tag == "H" || tag == "HPP" || tag == "CPP" || tag == "C" ||
                    tag == "LUA" || tag == "WGSL";
  const ImU32 tagColor = code ? IM_COL32(222, 200, 140, 255) : theme::kAccent;
  ImFont* font = ImGui::GetFont();
  const float fontSize = std::min(ImGui::GetFontSize() * 1.1f, h * 0.2f);
  const ImVec2 textSize =
      font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, tag.c_str());
  draw->AddText(font,
                fontSize,
                ImVec2(min.x + (w - textSize.x) * 0.5f, at(0, 0.58f).y),
                tagColor,
                tag.c_str());
}

// Files the Code panel edits: sources, build scripts, plain text.
bool isTextFile(const fs::path& path)
{
  static const std::vector<std::string> kExtensions = {".h",
                                                       ".hpp",
                                                       ".hh",
                                                       ".inl",
                                                       ".cpp",
                                                       ".cc",
                                                       ".cxx",
                                                       ".c",
                                                       ".cmake",
                                                       ".txt",
                                                       ".md",
                                                       ".lua",
                                                       ".json",
                                                       ".glsl",
                                                       ".wgsl"};
  const std::string extension = path.extension().string();
  return std::find(kExtensions.begin(), kExtensions.end(), extension) !=
         kExtensions.end();
}

bool isWithin(const fs::path& path, const fs::path& folder)
{
  return paths::within(path, folder);
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
  handler.ReadOpenFn =
      [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void*
  { return std::strcmp(name, "Panel") == 0 ? g_panel : nullptr; };
  handler.ReadLineFn =
      [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
  {
    if (auto* panel = static_cast<AssetBrowserPanel*>(entry))
      panel->readSetting(line);
  };
  handler.WriteAllFn =
      [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out)
  {
    if (g_panel)
      out->appendf(
          "[%s][Panel]\n%s\n", h->TypeName, g_panel->writeSettings().c_str());
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
  return "TileSize=" + std::to_string(static_cast<int>(m_tileSize)) +
         "\nGrid=" + (m_gridView ? "1" : "0") + "\n";
}

std::string AssetBrowserPanel::folderLabel(const fs::path& folder) const
{
  // The project's own folders (scenes, scripts, prefabs, assets) in title case.
  std::string name = label(folder);
  if (folder.parent_path() == m_root && !name.empty())
    name[0] =
        static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

// --- helpers
// ------------------------------------------------------------------------

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

fs::path AssetBrowserPanel::topFolderOf(const fs::path& path) const
{
  const std::optional<std::string> inside =
      m_root.empty() ? std::nullopt : paths::relative(path, m_root);
  if (!inside || inside->empty())
    return {};
  return m_root / inside->substr(0, inside->find('/'));
}

bool AssetBrowserPanel::isTopFolder(const fs::path& path) const
{
  return !m_root.empty() &&
         paths::normal(path) == paths::normal(topFolderOf(path));
}

std::optional<fs::path>
AssetBrowserPanel::selectedPath(const EditorContext& context) const
{
  if (context.assets && context.selectedAsset.valid())
    if (const sfs::AssetRecord* record =
            context.assets->find(context.selectedAsset))
      return m_root / record->path;
  if (!m_highlighted.empty())
    return m_highlighted;
  return std::nullopt;
}

void AssetBrowserPanel::newFolder(EditorContext& context,
                                  const fs::path& parent)
{
  // "New Folder" (numbered if taken), with its name ready to type.
  fs::path folder = parent / "New Folder";
  std::error_code error;
  for (int n = 2; fs::exists(folder, error); ++n)
    folder = parent / ("New Folder " + std::to_string(n));
  if (inCatalogue(parent) && context.assets)
  {
    if (!context.assets->createFolder(assetRelative(folder)))
      return;
  }
  else
  {
    fs::create_directories(folder, error);
    if (error)
    {
      LOG_ERROR("Can't make a folder in " + parent.string() + ": " +
                error.message());
      return;
    }
    requestRefresh();
  }
  m_highlighted = folder;
  beginRename(folder);
  recordMade("New Folder", {folder});
}

bool AssetBrowserPanel::movePaths(
    EditorContext& context,
    const std::vector<std::pair<fs::path, fs::path>>& moves)
{
  if (moves.empty())
    return false;
  // Within one of the project's folders (assets/ and prefabs/ count as one):
  // a script among the assets, say, would be lost to the build.
  const bool catalogue = inCatalogue(moves.front().first);
  const fs::path area = topFolderOf(moves.front().first);
  for (const auto& [from, to] : moves)
    if (isTopFolder(from) ||
        (catalogue ? !inCatalogue(to) : topFolderOf(to) != area) ||
        isWithin(to, from))
    {
      LOG_ERROR("Can't move " + from.filename().string() + " there" +
                (isTopFolder(from)
                     ? ": the project needs it where it is"
                     : ": it belongs in " + area.filename().string()));
      return false;
    }

  bool moved = true;
  if (catalogue && context.assets)
    for (const auto& [from, to] : moves)
    {
      if (fs::is_directory(from))
        moved &=
            context.assets->moveFolder(assetRelative(from), assetRelative(to));
      else if (const sfs::AssetRecord* record =
                   context.assets->findByPath(assetRelative(from)))
        moved &=
            gltfFiles::move(*context.assets, record->id, assetRelative(to));
    }
  else if (context.project && area == context.project->scriptsFolder())
    moved = scriptFiles::move(context.project->scriptsFolder(), moves);
  else
    for (const auto& [from, to] : moves)
    {
      std::error_code error;
      if (fs::exists(to, error))
      {
        LOG_ERROR("Can't move " + from.filename().string() +
                  ": there's already a " + to.filename().string());
        return false;
      }
      fs::rename(from, to, error);
      if (error)
      {
        LOG_ERROR("Can't move " + from.filename().string() + ": " +
                  error.message());
        return false;
      }
      if (m_actions.moved)
        m_actions.moved(from, to);
    }

  // The view follows what moved.
  for (const auto& [from, to] : moves)
  {
    m_open = paths::moved(m_open, from, to);
    if (!m_highlighted.empty())
      m_highlighted = paths::moved(m_highlighted, from, to);
  }
  if (!catalogue)
    requestRefresh();
  return moved;
}

std::optional<fs::path> AssetBrowserPanel::renameItem(EditorContext& context,
                                                      const fs::path& path,
                                                      std::string name)
{
  if (name.empty() || name.find_first_of("/\\") != std::string::npos)
    return std::nullopt;
  const bool directory = fs::is_directory(path);
  // Keep the file's type if the new name leaves the extension off.
  if (!directory && fs::path(name).extension().empty())
    name += path.extension().string();
  if (name == path.filename().string())
    return std::nullopt;
  const fs::path target = path.parent_path() / name;

  // A script named after its class: renaming it renames the class (in every
  // script), and its .h/.cpp pair goes with it.
  const bool scripts =
      context.project && topFolderOf(path) == context.project->scriptsFolder();
  const std::string from = path.stem().string(), to = target.stem().string();
  const auto isScriptSource = [](const fs::path& file)
  { return file.extension() == ".h" || file.extension() == ".cpp"; };
  if (scripts && !directory && isScriptSource(path) && from != to)
  {
    if (!scriptTemplates::isValidName(to))
    {
      LOG_ERROR(
          "'" + to +
          "' isn't a valid class name (a script is named after its class)");
      return std::nullopt;
    }
    if (scriptFiles::find(
            context.project->scriptsFolder(), to, scriptFiles::Prefer::Header))
    {
      LOG_ERROR("There's already a script called " + to);
      return std::nullopt;
    }
    std::vector<std::pair<fs::path, fs::path>> moves{{path, target}};
    for (const char* extension : {".h", ".cpp"})
    {
      const fs::path pair = path.parent_path() / (from + extension);
      std::error_code error;
      if (pair != path && fs::exists(pair, error))
        moves.push_back({pair, path.parent_path() / (to + extension)});
    }
    if (!movePaths(context, moves))
      return std::nullopt;
    const int files = scriptFiles::renameIdentifier(
        context.project->scriptsFolder(), from, to);
    LOG_INFO("Renamed the class " + from + " to " + to + " (" +
             std::to_string(files) + " script" + (files == 1 ? "" : "s") +
             "); scenes using it follow when the scripts are built");
    return target;
  }
  if (!movePaths(context, {{path, target}}))
    return std::nullopt;
  return target;
}

bool AssetBrowserPanel::moveItem(EditorContext& context,
                                 const fs::path& path,
                                 const fs::path& folder)
{
  // Not onto itself, its own folder, or (a folder) into itself.
  if (paths::normal(path.parent_path()) == paths::normal(folder) ||
      isWithin(folder, path))
    return false;
  const bool moved = movePaths(context, {{path, folder / path.filename()}});
  // Said in the Console, so a move made by a slip of the mouse is noticed.
  if (moved)
    LOG_INFO("Moved " + paths::relative(path, m_root).value_or(path.string()) +
             " to " +
             paths::relative(folder, m_root).value_or(folder.string()));
  return moved;
}

std::vector<fs::path>
AssetBrowserPanel::withCompanions(const fs::path& path) const
{
  std::vector<fs::path> out{path};
  std::error_code error;
  if (fs::is_directory(path, error) || !inCatalogue(path))
    return out; // a folder's .metas are inside it
  const auto add = [&](const fs::path& file)
  {
    if (fs::exists(file, error))
      out.push_back(file);
  };
  add(path.string() + ".meta");
  if (path.extension() == ".gltf")
  {
    const fs::path bin = path.parent_path() / (path.stem().string() + ".bin");
    add(bin);
    add(bin.string() + ".meta");
  }
  return out;
}

std::optional<AssetBrowserPanel::Trashed>
AssetBrowserPanel::toTrash(EditorContext& context, const fs::path& path)
{
  if (isTopFolder(path))
    return std::nullopt;
  // Into a trash folder of its own, at its place in the project (with its
  // .meta: undone, it comes back as the same asset, references and all).
  const fs::path bin =
      m_root / ".sfs" / "trash" / std::to_string(++m_trashCount);
  Trashed trashed;
  std::error_code error;
  for (const fs::path& from : withCompanions(path))
  {
    const fs::path to =
        bin / paths::relative(from, m_root).value_or(from.filename().string());
    fs::create_directories(to.parent_path(), error);
    fs::rename(from, to, error);
    if (error)
    {
      LOG_ERROR("Can't delete " + from.filename().string() + ": " +
                error.message());
      restore(context, trashed, true);
      return std::nullopt;
    }
    trashed.moves.push_back({from, to});
  }
  if (context.assets && context.selectedAsset.valid())
    if (const sfs::AssetRecord* record =
            context.assets->find(context.selectedAsset);
        record && isWithin(m_root / record->path, path))
      context.selectedAsset = {};
  if (isWithin(m_open, path))
    m_open = path.parent_path();
  if (isWithin(m_highlighted, path))
    m_highlighted.clear();
  if (inCatalogue(path) && context.assets)
    context.assets->refresh();
  requestRefresh();
  return trashed;
}

bool AssetBrowserPanel::restore(EditorContext& context,
                                const Trashed& trashed,
                                bool back)
{
  // Back where it was (undoing a delete), or into the trash again.
  std::error_code error;
  bool catalogue = false;
  for (const auto& [original, inTrash] : trashed.moves)
  {
    const fs::path& from = back ? inTrash : original;
    const fs::path& to = back ? original : inTrash;
    if (fs::exists(to, error))
    {
      LOG_ERROR("Can't put back " + to.filename().string() +
                ": there's one there now");
      return false;
    }
    fs::create_directories(to.parent_path(), error);
    fs::rename(from, to, error);
    if (error)
    {
      LOG_ERROR("Can't put back " + to.filename().string() + ": " +
                error.message());
      return false;
    }
    catalogue |= inCatalogue(original);
  }
  if (!back && !trashed.moves.empty())
  {
    const fs::path& gone = trashed.moves.front().first;
    if (isWithin(m_open, gone))
      m_open = gone.parent_path();
    if (isWithin(m_highlighted, gone))
      m_highlighted.clear();
  }
  if (catalogue && context.assets)
    context.assets->refresh();
  requestRefresh();
  return true;
}

std::vector<fs::path> AssetBrowserPanel::copyInto(EditorContext& context,
                                                  const fs::path& item,
                                                  const fs::path& folder)
{
  // Within one of the project's folders, as for moves.
  const bool catalogue = inCatalogue(item);
  if (isTopFolder(item) ||
      (catalogue ? !inCatalogue(folder)
                 : topFolderOf(folder) != topFolderOf(item)) ||
      (isWithin(folder, item) &&
       paths::normal(folder) != paths::normal(item.parent_path())))
  {
    LOG_ERROR("Can't paste " + item.filename().string() + " there" +
              (isTopFolder(item) ? ""
                                 : ": it belongs in " +
                                       topFolderOf(item).filename().string()));
    return {};
  }
  std::error_code error;
  if (!fs::exists(item, error))
  {
    LOG_ERROR("Can't paste " + item.filename().string() + ": it's gone");
    return {};
  }

  // Scripts: new classes.
  if (context.project && topFolderOf(item) == context.project->scriptsFolder())
  {
    std::vector<fs::path> made =
        scriptFiles::copy(context.project->scriptsFolder(), item, folder);
    requestRefresh();
    return made;
  }

  // "<name> Copy" where its name's taken.
  const bool directory = fs::is_directory(item, error);
  const std::string stem =
      directory ? item.filename().string() : item.stem().string();
  const std::string extension =
      directory ? std::string() : item.extension().string();
  fs::path target = folder / item.filename();
  for (int n = 1; fs::exists(target, error); ++n)
    target = folder / (stem + " Copy" + (n > 1 ? " " + std::to_string(n) : "") +
                       extension);

  // A new asset id for each copy (its import settings kept).
  const auto newId = [&](const fs::path& meta)
  {
    std::ifstream in(meta);
    nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object())
      return;
    json["id"] = sfs::AssetId::generate().toString();
    std::ofstream(meta) << json.dump(2) << "\n";
  };
  if (directory)
  {
    fs::copy(item, target, fs::copy_options::recursive, error);
    if (catalogue && !error)
      for (auto it = fs::recursive_directory_iterator(target, error);
           !error && it != fs::recursive_directory_iterator();
           it.increment(error))
        if (it->path().extension() == ".meta")
          newId(it->path());
  }
  else
  {
    fs::copy_file(item, target, error);
    if (catalogue && !error)
    {
      const auto copyMeta = [&](const fs::path& from, const fs::path& to)
      {
        std::error_code ignored;
        if (fs::copy_file(
                from.string() + ".meta", to.string() + ".meta", ignored))
          newId(to.string() + ".meta");
      };
      copyMeta(item, target);
      // A .gltf's .bin goes with it, renamed to match.
      const fs::path bin = item.parent_path() / (item.stem().string() + ".bin");
      if (item.extension() == ".gltf" && fs::exists(bin, error))
      {
        const fs::path newBin =
            target.parent_path() / (target.stem().string() + ".bin");
        fs::copy_file(bin, newBin, error);
        copyMeta(bin, newBin);
        std::ifstream in(target);
        nlohmann::json gltf = nlohmann::json::parse(in, nullptr, false);
        in.close();
        if (!gltf.is_discarded() && gltf.contains("buffers"))
        {
          for (nlohmann::json& buffer : gltf["buffers"])
            if (buffer.value("uri", std::string()) == bin.filename().string())
              buffer["uri"] = newBin.filename().string();
          std::ofstream(target) << gltf.dump(2);
        }
      }
    }
  }
  if (error)
  {
    LOG_ERROR("Can't copy " + item.filename().string() + ": " +
              error.message());
    return {};
  }
  if (catalogue && context.assets)
    context.assets->refresh();
  requestRefresh();
  return {target};
}

void AssetBrowserPanel::paste(EditorContext& context, const fs::path& folder)
{
  std::vector<fs::path> made;
  for (const fs::path& item : m_clipboard)
  {
    const std::vector<fs::path> copies = copyInto(context, item, folder);
    made.insert(made.end(), copies.begin(), copies.end());
  }
  if (made.empty())
    return;
  m_highlighted = made.front();
  recordMade("Paste " + made.front().filename().string(), made);
}

void AssetBrowserPanel::duplicate(EditorContext& context, const fs::path& item)
{
  const std::vector<fs::path> made =
      copyInto(context, item, item.parent_path());
  if (made.empty())
    return;
  m_highlighted = made.front();
  recordMade("Duplicate " + item.filename().string(), made);
}

void AssetBrowserPanel::recordMade(const std::string& label,
                                   const std::vector<fs::path>& made)
{
  // Undone, what was made goes to the trash; redone, it comes back.
  auto trashed = std::make_shared<std::vector<Trashed>>();
  record({label,
          [this, made, trashed](EditorContext& c)
          {
            if (trashed->empty())
            {
              for (const fs::path& path : made)
                if (const auto gone = toTrash(c, path))
                  trashed->push_back(*gone);
              return !trashed->empty();
            }
            bool ok = true;
            for (const Trashed& gone : *trashed)
              ok &= restore(c, gone, false);
            return ok;
          },
          [this, trashed](EditorContext& c)
          {
            bool ok = true;
            for (const Trashed& gone : *trashed)
              ok &= restore(c, gone, true);
            return ok;
          }});
}

bool AssetBrowserPanel::deleteItem(EditorContext& context, const fs::path& path)
{
  const std::optional<Trashed> trashed = toTrash(context, path);
  if (!trashed)
    return false;
  record({"Delete " + path.filename().string(),
          [this, gone = *trashed](EditorContext& c)
          { return restore(c, gone, true); },
          [this, gone = *trashed](EditorContext& c)
          { return restore(c, gone, false); }});
  return true;
}

void AssetBrowserPanel::record(FileOp op)
{
  m_undo.push_back(std::move(op));
  if (m_undo.size() > 100)
    m_undo.erase(m_undo.begin());
  m_redo.clear();
}

void AssetBrowserPanel::undo(EditorContext& context)
{
  if (m_undo.empty())
    return;
  FileOp op = std::move(m_undo.back());
  m_undo.pop_back();
  if (op.undo(context))
  {
    LOG_INFO("Undid " + op.label);
    m_redo.push_back(std::move(op));
  }
  else
    LOG_ERROR("Couldn't undo " + op.label);
}

void AssetBrowserPanel::redo(EditorContext& context)
{
  if (m_redo.empty())
    return;
  FileOp op = std::move(m_redo.back());
  m_redo.pop_back();
  if (op.redo(context))
  {
    LOG_INFO("Redid " + op.label);
    m_undo.push_back(std::move(op));
  }
  else
    LOG_ERROR("Couldn't redo " + op.label);
}

void AssetBrowserPanel::duplicateSelected(EditorContext& context)
{
  if (const std::optional<fs::path> selected = selectedPath(context);
      selected && !isTopFolder(*selected))
    duplicate(context, *selected);
}

void AssetBrowserPanel::deleteSelected(EditorContext& context)
{
  if (const std::optional<fs::path> selected = selectedPath(context);
      selected && !isTopFolder(*selected))
    m_pendingDelete = *selected;
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
  // Deleted things are kept for undo while the project's open, no longer.
  std::error_code error;
  fs::remove_all(m_root / ".sfs" / "trash", error);
  m_undo.clear();
  m_redo.clear();
  m_clipboard.clear();
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
                      [root = m_scanRoot,
                       mode = context.assets->mode(),
                       folders = context.assets->scannedFolders()] {
                        return sfs::AssetDatabase::scan(root, mode, folders);
                      });
}

const std::vector<AssetBrowserPanel::Entry>&
AssetBrowserPanel::listFolder(const EditorContext& context,
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
  const auto finish =
      [&](std::vector<Entry> entries) -> const std::vector<Entry>&
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
      if (record.path.parent_path() == relative &&
          !gltfFiles::isCompanion(*context.assets, record))
        entries.push_back({m_root / record.path, false, record.id});
    return finish(std::move(entries));
  }

  // scenes/, scripts/: the folder itself.
  std::vector<Entry> entries;
  std::error_code error;
  for (const auto& item : fs::directory_iterator(folder, error))
    if (!label(item.path()).starts_with("."))
      entries.push_back({item.path(), item.is_directory(error)});
  std::sort(entries.begin(),
            entries.end(),
            [](const Entry& a, const Entry& b)
            {
              return a.directory != b.directory
                         ? a.directory
                         : a.path.filename() < b.path.filename();
            });
  return finish(std::move(entries));
}

const sfs::AssetRecord*
AssetBrowserPanel::recordOf(const EditorContext& context,
                            const Entry& entry) const
{
  return entry.asset.valid() && context.assets
             ? context.assets->find(entry.asset)
             : nullptr;
}

// --- drawing
// ------------------------------------------------------------------------

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
  if (m_scan.valid() &&
      m_scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
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
    if (const sfs::AssetRecord* record =
            context.assets->find(context.revealAsset))
    {
      m_open = m_root / record->path.parent_path();
      m_scrollTo = record->id;
      m_revealTree = true;
      ImGui::SetWindowFocus();
    }
    context.revealAsset = {};
  }
  if (!context.revealFile.empty())
  {
    const fs::path file = std::exchange(context.revealFile, {});
    if (isWithin(file, m_root))
    {
      m_open = file.parent_path();
      m_scrollToFile = file;
      m_highlighted = file;
      m_revealTree = true;
      ImGui::SetWindowFocus();
    }
  }
  if (std::exchange(context.assetFilesChanged, false))
    m_thumbnails.recheck();

  m_thumbnails.beginFrame();

  // Whose the Edit menu is: this panel's while it, or one of its parts, was
  // the last panel focused (menus and popups don't count).
  if (const ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow)
    if (!(focused->Flags &
          (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu |
           ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_Tooltip)))
      m_ownsEdit = focused->RootWindowDockTree ==
                   ImGui::GetCurrentWindow()->RootWindowDockTree;

  // Keys, while this panel has focus (elsewhere they're the scene's).
  if (!m_renaming && !ImGui::GetIO().WantTextInput)
  {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z))
      undo(context);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
      redo(context);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V))
      paste(context, m_open);
    const std::optional<fs::path> selected = selectedPath(context);
    const bool any = selected && !isTopFolder(*selected);
    if (ImGui::Shortcut(ImGuiKey_F2) && any)
      beginRename(*selected);
    if (ImGui::Shortcut(ImGuiKey_Delete) && any)
      m_pendingDelete = *selected;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C) && any)
      m_clipboard = {*selected};
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D) && any)
      duplicate(context, *selected);
  }

  drawToolbar(context);
  ImGui::Separator();

  // Folder tree | contents, side by side.
  const float treeWidth =
      std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.25f);
  ImGui::BeginChild(
      "Folders", ImVec2(treeWidth, 0.0f), ImGuiChildFlags_ResizeX);
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
  ImGui::SetItemTooltip(m_gridView ? "Show the folder as a list"
                                   : "Show the folder as tiles with previews");
  if (m_gridView)
  {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    // As tall as the small buttons beside it.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
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

void AssetBrowserPanel::drawFolderTree(EditorContext& context,
                                       const fs::path& folder)
{
  std::vector<Entry> subfolders;
  for (const Entry& entry : listFolder(context, folder))
    if (entry.directory)
      subfolders.push_back(entry);

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                             ImGuiTreeNodeFlags_SpanAvailWidth;
  if (subfolders.empty())
    flags |= ImGuiTreeNodeFlags_Leaf;
  if (folder == m_open)
    flags |= ImGuiTreeNodeFlags_Selected;
  if (isWithin(m_open, folder) && folder != m_open)
    ImGui::SetNextItemOpen(
        true, m_revealTree ? ImGuiCond_Always : ImGuiCond_Once);

  const bool open = ImGui::TreeNodeEx(folder.generic_string().c_str(),
                                      flags,
                                      "%s",
                                      folderLabel(folder).c_str());
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    m_open = folder;
  dragSource(context, {folder, true}); // not the project's own folders
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
  else if (ImGui::BeginTable(
               "Entries",
               2,
               ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
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
  if (ImGui::BeginPopupContextWindow(
          "FolderBackground",
          ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
  {
    drawFolderMenu(context, m_open);
    ImGui::EndPopup();
  }

  // Dropping an entity anywhere in the open folder makes a prefab of it.
  if (inCatalogue(m_open))
  {
    const ImRect area(
        ImGui::GetWindowPos(),
        ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
               ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    if (ImGui::BeginDragDropTargetCustom(area, ImGui::GetID("FolderDrop")))
    {
      acceptEntity(context, m_open);
      ImGui::EndDragDropTarget();
    }
  }
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

  // An asset is highlighted when selected, or (with none selected, as
  // selectedPath() prefers one) when something asked to see its file (an
  // Inspector field's find icon), as a plain file is.
  const bool selected =
      (record && record->id == context.selectedAsset) ||
      (!context.selectedAsset.valid() && entry.path == m_highlighted);
  const std::string name = (entry.directory ? "[+] " : "") + entry.name;
  const bool clicked =
      ImGui::Selectable(name.c_str(),
                        selected,
                        ImGuiSelectableFlags_SpanAllColumns |
                            ImGuiSelectableFlags_AllowDoubleClick);
  if ((entry.asset.valid() && entry.asset == m_scrollTo) ||
      (!m_scrollToFile.empty() && entry.path == m_scrollToFile))
  {
    ImGui::SetScrollHereY(0.5f);
    m_scrollTo = {};
    m_scrollToFile.clear();
  }
  entryBehaviour(context,
                 entry,
                 record,
                 clicked,
                 clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));

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
  if (clicked)
    m_highlighted = record
                        ? fs::path()
                        : entry.path; // files outside the catalogue select too
  if (doubleClicked)
    entry.directory ? void(m_open = entry.path) : activate(context, entry.path);
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
    if (!record)
      m_highlighted = entry.path;
    if (!entry.directory && ImGui::MenuItem("Open"))
      activate(context, entry.path);
    if (!isTopFolder(entry.path))
    {
      if (ImGui::MenuItem("Copy", "Ctrl+C"))
        m_clipboard = {entry.path};
      if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
        duplicate(context, entry.path);
      if (entry.directory &&
          ImGui::MenuItem("Paste", "Ctrl+V", false, !m_clipboard.empty()))
        paste(context, entry.path);
      ImGui::Separator();
      if (ImGui::MenuItem("Rename", "F2"))
        beginRename(entry.path);
      if (ImGui::MenuItem("Delete...", "Del"))
        m_pendingDelete = entry.path;
    }
    if (ImGui::MenuItem("Show in Folder"))
      entry.directory ? shell::openExternally(entry.path)
                      : shell::showInFolder(entry.path);
    ImGui::EndPopup();
  }
}

void AssetBrowserPanel::drawGrid(EditorContext& context,
                                 const std::vector<Entry>& entries)
{
  const ImGuiStyle& style = ImGui::GetStyle();
  const float spacing = style.ItemSpacing.x;
  const int columns =
      std::max(1,
               static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) /
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

  // Selected when let go of, unless it was dragged somewhere (dropped on the
  // Inspector, which mustn't switch to showing it), or on a double-click's
  // second release (after it has opened something).
  ImGui::InvisibleButton("tile", size);
  const bool clicked =
      ImGui::IsItemDeactivated() &&
      !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left) &&
      ImGui::GetIO().MouseClickedLastCount[ImGuiMouseButton_Left] < 2;
  if ((entry.asset.valid() && entry.asset == m_scrollTo) ||
      (!m_scrollToFile.empty() && entry.path == m_scrollToFile))
  {
    ImGui::SetScrollHereY(0.5f);
    m_scrollTo = {};
    m_scrollToFile.clear();
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool doubleClicked =
      hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  entryBehaviour(context, entry, record, clicked, doubleClicked);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  // An asset is highlighted when selected, or (with none selected, as
  // selectedPath() prefers one) when something asked to see its file (an
  // Inspector field's find icon), as a plain file is.
  const bool selected =
      (record && record->id == context.selectedAsset) ||
      (!context.selectedAsset.valid() && entry.path == m_highlighted);
  const ImVec2 end(at.x + size.x, at.y + size.y);
  if (selected)
  {
    draw->AddRectFilled(at, end, theme::kAccentSoft, 7.0f);
    draw->AddRect(at, end, theme::kAccent, 7.0f, 0, 1.5f);
  }
  else if (hovered)
    draw->AddRectFilled(at, end, theme::kTileHovered, 7.0f);

  const ImVec2 pictureMin(at.x + padding, at.y + padding);
  const ImVec2 pictureMax(
      at.x + m_tileSize - padding, at.y + m_tileSize - padding);
  const ImTextureID preview =
      record && context.scene ? m_thumbnails.get(*record,
                                                 *context.assets,
                                                 context.scene->assetStore(),
                                                 context.scene)
                              : ImTextureID{};
  if (preview)
  {
    draw->AddRectFilled(pictureMin, pictureMax, theme::kTile, 5.0f);
    draw->AddImageRounded(preview,
                          pictureMin,
                          pictureMax,
                          ImVec2(0, 0),
                          ImVec2(1, 1),
                          IM_COL32_WHITE,
                          5.0f);
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
      draw->AddText(ImVec2(textAt.x + (textWidth - width) * 0.5f,
                           textAt.y + lineHeight * line),
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
  const bool done = widgets::inputText(
      "##rename",
      m_renameText,
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
      (!done && ImGui::IsItemDeactivated()))
  {
    m_renaming.reset();
    return false;
  }
  if (!done)
    return false;

  m_renaming.reset();
  const fs::path before = entry.path;
  const std::optional<fs::path> after =
      renameItem(context, before, m_renameText);
  if (after)
    record(
        {"Rename " + before.filename().string(),
         [this, before, after = *after](EditorContext& c) {
           return renameItem(c, after, before.filename().string()).has_value();
         },
         [this, before, after = *after](EditorContext& c) {
           return renameItem(c, before, after.filename().string()).has_value();
         }});
  return after.has_value();
}

void AssetBrowserPanel::drawFolderMenu(EditorContext& context,
                                       const fs::path& folder)
{
  if (ImGui::MenuItem("New Folder"))
    newFolder(context, folder);
  if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_clipboard.empty()))
    paste(context, folder);

  // The scripts folder, or one inside it: new scripts go here.
  if (context.project && isWithin(folder, context.project->scriptsFolder()))
  {
    using scriptTemplates::Kind;
    for (const Kind kind : {Kind::GameObject,
                            Kind::System,
                            Kind::Component,
                            Kind::ScriptableObject})
    {
      const std::string itemLabel =
          std::string("New ") + scriptTemplates::label(kind) + "...";
      if (ImGui::MenuItem(itemLabel.c_str()) && m_actions.newScript)
        m_actions.newScript(kind, folder);
    }
  }
  if (inCatalogue(folder) && context.assets)
  {
    // Something new here: selected, with its name ready to type.
    const auto made = [&](const std::optional<sfs::AssetId>& id)
    {
      if (const sfs::AssetRecord* record =
              id ? context.assets->find(*id) : nullptr)
      {
        context.selectedAsset = *id;
        context.selection = {};
        beginRename(m_root / record->path);
      }
    };
    if (ImGui::MenuItem("New Material"))
      made(materialFiles::create(
          *context.assets, assetRelative(folder), "New Material"));
    if (ImGui::MenuItem("New Material Graph"))
    {
      sfs::MaterialAsset material;
      material.graph = sfs::materialGraph::Graph::makeDefault();
      made(materialFiles::create(
          *context.assets, assetRelative(folder), "New Material", material));
    }
    ImGui::SetItemTooltip("A material whose look you build from nodes "
                          "(double-click it to edit them)");
    // One of the scripts' scriptable object types (sfs::ScriptableObject).
    if (ImGui::BeginMenu("New Scriptable Object"))
    {
      bool any = false;
      for (const sfs::reflect::TypeInfo& type : sfs::reflect::types().types())
        if (type.kind == sfs::reflect::TypeKind::ScriptableObject)
        {
          any = true;
          if (ImGui::MenuItem(type.displayName.c_str()))
            made(scriptableObjectFiles::create(
                *context.assets,
                assetRelative(folder),
                type,
                "New " + type.displayName,
                context.render ? &context.render->meshes() : nullptr));
        }
      if (!any)
        ImGui::TextDisabled("No scriptable object types yet: derive one from "
                            "sfs::ScriptableObject in scripts");
      ImGui::EndMenu();
    }
  }
  if (!isTopFolder(folder))
  {
    ImGui::Separator();
    if (ImGui::MenuItem("Copy"))
      m_clipboard = {folder};
    if (ImGui::MenuItem("Duplicate"))
      duplicate(context, folder);
    if (ImGui::MenuItem("Rename"))
      beginRename(folder);
    if (ImGui::MenuItem("Delete..."))
      m_pendingDelete = folder;
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Show in Folder"))
    shell::openExternally(folder);
}

void AssetBrowserPanel::drawDeleteConfirmation(EditorContext& context)
{
  constexpr const char* kPopup = "Delete?";
  if (m_pendingDelete && !ImGui::IsPopupOpen(kPopup))
    ImGui::OpenPopup(kPopup);
  if (!ImGui::BeginPopupModal(
          kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;

  const fs::path path = m_pendingDelete.value_or(fs::path());
  const bool folder = fs::is_directory(path);
  ImGui::Text("Delete %s%s?",
              label(path).c_str(),
              folder ? " and everything in it" : "");
  const bool scripts =
      context.project && topFolderOf(path) == context.project->scriptsFolder();
  ImGui::TextDisabled(
      scripts ? "Scripts that include it won't build until they're changed."
              : "Anything using it loses it.");
  ImGui::TextDisabled("Undo (Ctrl+Z here) brings it back.");
  ImGui::Spacing();
  if (ImGui::Button("Delete"))
  {
    deleteItem(context, path);
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

// --- drag and drop
// ---------------------------------------------------------------------

void AssetBrowserPanel::dragSource(const EditorContext&, const Entry& entry)
{
  if (isTopFolder(entry.path) || m_renaming == entry.path ||
      !ImGui::BeginDragDropSource())
    return;
  // As the system writes it: generic_string() can fold a network share's
  // leading "//" (\\wsl.localhost\...) into "/".
  const std::string path = entry.path.string();
  ImGui::SetDragDropPayload(kAssetPayload, path.c_str(), path.size() + 1);
  ImGui::TextUnformatted(label(entry.path).c_str());
  ImGui::EndDragDropSource();
}

void AssetBrowserPanel::acceptEntity(EditorContext& context,
                                     const fs::path& folder)
{
  // An entity dragged from the Hierarchy: made into a prefab here.
  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  if (!peek || !peek->IsDataType(assetDrop::kEntityPayload) || !context.scene ||
      !ImGui::AcceptDragDropPayload(assetDrop::kEntityPayload))
    return;
  const sfs::Entity entity = context.scene->getEntity(
      *static_cast<const sfs::Entity::EntityId*>(peek->Data));
  if (entity.isValid() && entity.isAlive())
    if (const auto id = prefabs::create(context, entity, assetRelative(folder)))
      context.selectedAsset = {};
}

void AssetBrowserPanel::dropTarget(EditorContext& context,
                                   const fs::path& folder)
{
  if (!ImGui::BeginDragDropTarget())
    return;
  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  if (inCatalogue(folder) && context.assets)
    acceptEntity(context, folder);
  if (peek && peek->IsDataType(kAssetPayload))
  {
    const fs::path dragged(static_cast<const char*>(peek->Data));
    // Somewhere it can go: not its own folder or inside itself, and in the
    // same project folder (assets/ and prefabs/ together).
    const bool sameArea = inCatalogue(dragged)
                              ? inCatalogue(folder)
                              : topFolderOf(dragged) == topFolderOf(folder);
    const bool allowed =
        sameArea &&
        paths::normal(dragged.parent_path()) != paths::normal(folder) &&
        !isWithin(folder, dragged);
    if (allowed && ImGui::AcceptDragDropPayload(kAssetPayload) &&
        moveItem(context, dragged, folder))
    {
      const fs::path moved = folder / dragged.filename();
      record({"Move " + dragged.filename().string(),
              [this, dragged, moved](EditorContext& c)
              { return moveItem(c, moved, dragged.parent_path()); },
              [this, dragged, folder](EditorContext& c)
              { return moveItem(c, dragged, folder); }});
    }
  }
  ImGui::EndDragDropTarget();
}

// --- actions
// ---------------------------------------------------------------------------

void AssetBrowserPanel::beginRename(const fs::path& path)
{
  // The name is edited in place in the contents list, so show its folder.
  m_open = path.parent_path();
  m_renaming = path;
  m_renameText = label(path);
}

void AssetBrowserPanel::activate(EditorContext& context, const fs::path& file)
{
  const sfs::AssetKind kind = sfs::assetKindOf(file);
  if (kind == sfs::AssetKind::Material)
  {
    // A node-graph material opens in its own tab; a standard one is edited
    // in the Inspector, which selecting it shows.
    const sfs::AssetRecord* record =
        context.assets ? context.assets->findByPath(assetRelative(file))
                       : nullptr;
    const std::optional<sfs::MaterialAsset> material =
        record ? materialFiles::read(*context.assets, *record) : std::nullopt;
    if (material && material->graph)
      context.openMaterial = record->id;
    return;
  }
  if (kind == sfs::AssetKind::ScriptableObject)
  {
    // Its values are edited in the Inspector; opening it shows its type's
    // declaration, to change what it holds.
    const std::optional<nlohmann::json> contents =
        sfs::SceneSerializer::readJsonFile(file);
    const std::string type = contents && contents->is_object()
                                 ? contents->value("type", std::string())
                                 : "";
    const auto header =
        context.project && !type.empty()
            ? scriptFiles::find(context.project->scriptsFolder(),
                                type,
                                scriptFiles::Prefer::Header)
            : std::nullopt;
    if (header && m_actions.openText)
      m_actions.openText(*header);
    else
      LOG_ERROR("Can't find the declaration of " +
                (type.empty() ? file.filename().string() + "'s type" : type) +
                " in the scripts folder");
    return;
  }
  if (kind == sfs::AssetKind::Prefab)
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
