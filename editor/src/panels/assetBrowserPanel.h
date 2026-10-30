#pragma once

#include "assets/thumbnails.h"
#include "panels/panel.h"
#include "scripting/scriptTemplates.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Browses the project's content: a folder tree (scenes, scripts, assets) on the
// left and the open folder's contents on the right, as tiles with previews
// (textures, materials and models; see Thumbnails) or as a list.
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
    std::function<void(const std::filesystem::path&)> openPrefab;
    std::function<void(scriptTemplates::Kind, const std::filesystem::path& folder)> newScript;
    // A file or folder moved or renamed in the project, outside the asset
    // catalogue (e.g. a scene: the open one, or the start scene, follow it).
    std::function<void(const std::filesystem::path& from, const std::filesystem::path& to)> moved;
  };

  AssetBrowserPanel(Actions actions, sfs::IRenderBackend& backend);
  ~AssetBrowserPanel() override;

  // Its view settings (tile size, grid or list), kept in imgui.ini.
  void readSetting(const char* line);
  std::string writeSettings() const;

  /**
   * Where dropped files are imported: the open folder if it's in assets/ or
   * prefabs/, else assets/ itself (relative to the project).
   */
  std::filesystem::path importFolder() const;

  /**
   * Read the folders again next frame (the editor regained focus: files may
   * have changed outside it). Reading a project on a network share (WSL) takes
   * a while, so it's done on request rather than continually.
   */
  void requestRefresh() { m_refreshRequested = true; }

  /**
   * Whether Undo, Redo, Duplicate and Delete (the Edit menu, Ctrl+Z...) are
   * this panel's: it had focus last, of the editor's panels.
   */
  bool ownsEdit() const { return m_ownsEdit; }

  // Undoing the file operations done here (new folder, rename, move, delete,
  // paste, duplicate).
  bool canUndo() const { return !m_undo.empty(); }
  bool canRedo() const { return !m_redo.empty(); }
  /** e.g. "Rename Wheat.asset" (empty: nothing). */
  std::string undoLabel() const { return m_undo.empty() ? std::string() : m_undo.back().label; }
  std::string redoLabel() const { return m_redo.empty() ? std::string() : m_redo.back().label; }
  void undo(EditorContext& context);
  void redo(EditorContext& context);
  /** Duplicate or delete (after asking) what's selected here. */
  void duplicateSelected(EditorContext& context);
  void deleteSelected(EditorContext& context);

  /** Previews being made in the background (for a progress bar). */
  Thumbnails::Progress previewProgress() const { return m_thumbnails.progress(); }

  /** Whether the folders are being read again (in the background). */
  bool refreshing() const { return m_scan.valid(); }

  void update(EditorContext& context) override;

protected:
  void drawContents(EditorContext& context) override;

private:
  // One row: a folder or a file, in the tree or the contents.
  struct Entry
  {
    std::filesystem::path path; // absolute
    bool directory = false;
    // Worked out once per listing (path handling is slow next to drawing).
    sfs::AssetId asset; // in assets/: its record's id
    std::string id;     // the path, generic: its ImGui id
    std::string name;   // label(path)
  };

  // Read the folders again, in the background.
  void refresh(EditorContext& context);
  // A newly opened project: start the view afresh.
  void showProject(EditorContext& context);
  // The project's top folders and scenes/scripts listings, read again.
  void readFolders();
  const std::vector<Entry>& listFolder(const EditorContext& context, const std::filesystem::path& folder);
  // An entry's catalogue record (assets/ files), else null.
  const sfs::AssetRecord* recordOf(const EditorContext& context, const Entry& entry) const;

  void drawToolbar(EditorContext& context);
  void drawFolderTree(EditorContext& context, const std::filesystem::path& folder);
  void drawContentsPane(EditorContext& context);
  void drawEntry(EditorContext& context, const Entry& entry); // a list row
  void drawGrid(EditorContext& context, const std::vector<Entry>& entries);
  void drawTile(EditorContext& context, const Entry& entry);
  // What clicking, double-clicking, dragging, dropping and right-clicking do
  // to an entry, for the item just drawn for it (a row or a tile).
  void entryBehaviour(EditorContext& context,
                      const Entry& entry,
                      const sfs::AssetRecord* record,
                      bool clicked,
                      bool doubleClicked);
  bool drawRename(EditorContext& context, const Entry& entry);
  void drawFolderMenu(EditorContext& context, const std::filesystem::path& folder);
  void drawDeleteConfirmation(EditorContext& context);

  // File operations, the same in every folder: through the asset catalogue
  // in assets/ and prefabs/, keeping the scripts building in scripts/, and
  // telling the editor (Actions::moved) elsewhere. The project's own folders
  // (scripts/, assets/...) can't be renamed, moved or deleted.
  void newFolder(EditorContext& context, const std::filesystem::path& parent);
  // @return the new path; nothing (logged) if it can't be renamed
  std::optional<std::filesystem::path> renameItem(EditorContext& context, const std::filesystem::path& path,
                                                  std::string name);
  bool moveItem(EditorContext& context, const std::filesystem::path& path, const std::filesystem::path& folder);
  bool deleteItem(EditorContext& context, const std::filesystem::path& path);
  // Copies: into a folder (the copy named "<name> Copy" where the name's
  // taken; scripts as new classes). @return what was made
  std::vector<std::filesystem::path> copyInto(EditorContext& context, const std::filesystem::path& item,
                                              const std::filesystem::path& folder);
  void paste(EditorContext& context, const std::filesystem::path& folder);
  void duplicate(EditorContext& context, const std::filesystem::path& item);

  // Deleted things wait in the project's trash (.sfs/trash), so deleting
  // can be undone: each moved path, and where it went.
  struct Trashed
  {
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moves; // from -> in the trash
  };
  std::optional<Trashed> toTrash(EditorContext& context, const std::filesystem::path& path);
  bool restore(EditorContext& context, const Trashed& trashed, bool back);
  // A file and what goes with it: its .meta, a .gltf's .bin (and its .meta).
  std::vector<std::filesystem::path> withCompanions(const std::filesystem::path& path) const;

  // An undoable operation, as how to undo and redo it.
  struct FileOp
  {
    std::string label;
    std::function<bool(EditorContext&)> undo, redo;
  };
  void record(FileOp op);
  // Undone, what an operation made goes to the trash; redone, it's back.
  void recordMade(const std::string& label, const std::vector<std::filesystem::path>& made);

  // Several moves at once, all in one of the project's folders (assets/ and
  // prefabs/ count as one).
  bool movePaths(EditorContext& context, const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& moves);
  // The project folder a path is in (e.g. <project>/scripts), and whether it's one.
  std::filesystem::path topFolderOf(const std::filesystem::path& path) const;
  bool isTopFolder(const std::filesystem::path& path) const;
  // What's selected here, for F2 and Delete: an asset, or another file or folder.
  std::optional<std::filesystem::path> selectedPath(const EditorContext& context) const;

  // Drag and drop: any file or folder (but the project's own) onto a folder.
  void dragSource(const EditorContext& context, const Entry& entry);
  void dropTarget(EditorContext& context, const std::filesystem::path& folder);
  // Inside a drop target: an entity from the Hierarchy becomes a prefab here.
  void acceptEntity(EditorContext& context, const std::filesystem::path& folder);

  void activate(EditorContext& context, const std::filesystem::path& file);
  void beginRename(const std::filesystem::path& path);

  // Whether a path is in a catalogued folder (assets/, prefabs/), and its path
  // in the catalogue (relative to the project, e.g. "assets/models").
  bool inCatalogue(const std::filesystem::path& path) const;
  std::filesystem::path assetRelative(const std::filesystem::path& path) const;

  Actions m_actions;
  std::filesystem::path m_root;   // the project
  std::filesystem::path m_assets; // its assets/ folder
  std::filesystem::path m_open;   // the folder whose contents are shown
  bool m_refreshRequested = false;
  std::future<sfs::AssetDatabase::Scan> m_scan; // a refresh under way
  std::filesystem::path m_scanRoot;             // the catalogue it's for
  sfs::AssetId m_scrollTo;  // an asset to scroll into view (revealed)
  std::filesystem::path m_scrollToFile; // the same for a file outside the catalogue
  std::filesystem::path m_highlighted;  // a file outside the catalogue shown selected (scripts...)
  bool m_revealTree = false; // open the folder tree down to m_open, this frame
  std::vector<std::filesystem::path> m_topFolders; // of kFolders, those that exist
  // scenes/ and scripts/ listings, read once per refresh (assets/ comes from
  // the AssetDatabase, which is kept in memory).
  std::map<std::filesystem::path, std::vector<Entry>> m_listings;
  uint64_t m_listingsVersion = 0; // the catalogue's version they were made from

  std::optional<std::filesystem::path> m_renaming;
  std::string m_renameText;
  std::optional<std::filesystem::path> m_pendingDelete;
  std::vector<std::filesystem::path> m_clipboard; // what Copy took, for Paste
  std::vector<FileOp> m_undo, m_redo;
  bool m_ownsEdit = false;
  int m_trashCount = 0; // names the next trash folder

  bool m_gridView = true;  // tiles with previews, or a list
  float m_tileSize = 96.0f; // a tile's picture, in pixels
  static constexpr float kMinTile = 64.0f;
  static constexpr float kMaxTile = 160.0f;
  // A folder's name as shown: the project's own folders title-cased.
  std::string folderLabel(const std::filesystem::path& folder) const;
  Thumbnails m_thumbnails;
};
