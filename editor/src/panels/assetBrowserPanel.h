#pragma once

#include "panels/panel.h"
#include "scripting/scriptTemplates.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Browses the project's content: a folder tree (scenes, scripts, assets) on the
// left and the open folder's contents on the right.
//
// Everything under assets/ goes through the project's AssetDatabase, so an
// asset keeps its id however it's reorganised: select an asset to see it in the
// Inspector; rename (F2 or right-click), delete, drag onto a folder to move,
// right-click for New Folder; drop files from the system onto the window to
// import them into the open folder. scenes/ and scripts/ are listed as files.
//
// Double-click a scene to open it, a script or other text file to edit it in
// the Code panel, and anything else to open it in the system's default
// application.
//
// Listings are refreshed every couple of seconds (or on demand), since walking
// folders every frame is slow on network/WSL shares.
class AssetBrowserPanel : public Panel
{
public:
  struct Actions
  {
    std::function<void(const std::filesystem::path&)> openScene;
    std::function<void(const std::filesystem::path&)> openText;
    std::function<void(scriptTemplates::Kind)> newScript;
  };

  explicit AssetBrowserPanel(Actions actions)
      : Panel("Assets"), m_actions(std::move(actions))
  {
  }

  /**
   * Where dropped files are imported: the open folder if it's in assets/,
   * else assets/ itself (relative to assets/).
   */
  std::filesystem::path importFolder() const;

protected:
  void drawContents(EditorContext& context) override;

private:
  // One row: a folder or a file, in the tree or the contents.
  struct Entry
  {
    std::filesystem::path path; // absolute
    bool directory = false;
  };

  void refresh(EditorContext& context);
  std::vector<Entry> listFolder(const EditorContext& context, const std::filesystem::path& folder);

  void drawToolbar(EditorContext& context);
  void drawFolderTree(EditorContext& context, const std::filesystem::path& folder);
  void drawContentsPane(EditorContext& context);
  void drawEntry(EditorContext& context, const Entry& entry);
  bool drawRename(EditorContext& context, const Entry& entry);
  void drawFolderMenu(EditorContext& context, const std::filesystem::path& folder);
  void drawDeleteConfirmation(EditorContext& context);

  // Folder and asset drag and drop (assets/ only).
  void dragSource(const EditorContext& context, const Entry& entry);
  void dropTarget(EditorContext& context, const std::filesystem::path& folder);

  void activate(const std::filesystem::path& file);
  void beginRename(const std::filesystem::path& path);

  bool inAssets(const std::filesystem::path& path) const;
  std::filesystem::path assetRelative(const std::filesystem::path& path) const;

  Actions m_actions;
  std::filesystem::path m_root;   // the project
  std::filesystem::path m_assets; // its assets/ folder
  std::filesystem::path m_open;   // the folder whose contents are shown
  std::chrono::steady_clock::time_point m_lastRefresh;
  // scenes/ and scripts/ listings, read once per refresh (assets/ comes from
  // the AssetDatabase, which is kept in memory).
  std::map<std::filesystem::path, std::vector<Entry>> m_listings;

  std::optional<std::filesystem::path> m_renaming;
  std::string m_renameText;
  std::optional<std::filesystem::path> m_pendingDelete;
  std::string m_newFolderName;
};
