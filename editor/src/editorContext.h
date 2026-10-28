#pragma once

#include "project/project.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/vector_int4.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace sfs
{
class Scene;
class RenderSystem3D;
} // namespace sfs

class EditorHistory;
class EditorScene;
class SceneDocument;

// The editor state every panel reads and edits: the open project, the scene
// being edited, and the current selection.
struct EditorContext
{
  std::optional<Project> project;

  EditorScene* scene = nullptr;
  sfs::RenderSystem3D* render = nullptr;
  SceneDocument* document = nullptr;

  // Every scene edit goes through here so it can be undone.
  EditorHistory* history = nullptr;

  // While playing, edits are temporary (Stop restores the scene).
  bool playing = false;

  // A panel (the code editor) owns the keyboard: editor shortcuts stand down.
  bool keyboardCaptured = false;

  // While playing: the game gets input (the viewport has focus), and mouse
  // presses and the wheel (the pointer is over the game's image).
  bool gameHasInput = false;
  bool gameHasMouse = false;

  // Give the viewport focus next frame (e.g. Play started), so the game has
  // the keyboard without a click first.
  bool focusViewport = false;

  // Where the viewport shows the scene, so the game's mouse positions can be
  // made relative to it: the OS window it's in (SDL window id; the panel may
  // be dragged out into its own) and the image's rectangle in that window, in
  // window pixels (x, y, width, height).
  uint32_t viewportWindowId = 0;
  glm::ivec4 viewportRect{0};

  // The selected entity; invalid (default) when nothing is selected.
  sfs::Entity selection;

  // Rewrites a prefab's file (`write`, false on failure), then reloads the
  // open scenes so their instances show it. Set by the app.
  std::function<bool(const sfs::AssetId& prefab, const std::function<bool()>& write)> rewritePrefab;

  // A prefab to edit in its own tab; the app does it next frame.
  sfs::AssetId openPrefab;

  // A text file (a script) to open in the code panel; the app does it next
  // frame.
  std::filesystem::path openText;

  // An entity for the viewport to frame (any panel may ask; the viewport does
  // it next frame and clears this).
  sfs::Entity focusRequest;

  // The project's asset catalogue (its assets/ folder); null without a project.
  sfs::AssetDatabase* assets = nullptr;

  // An asset file was rewritten here (a material saved): the Assets panel's
  // previews check their files again.
  bool assetFilesChanged = false;

  // An asset for the Assets panel to show: it opens its folder and scrolls
  // to it (e.g. the Inspector's Select Asset). Cleared once done.
  sfs::AssetId revealAsset;
  // The same for a file outside the catalogue (a script), by its path.
  std::filesystem::path revealFile;

  // The asset selected in the Assets panel (invalid when none). The Inspector
  // shows it while no entity is selected; selecting an entity clears it.
  sfs::AssetId selectedAsset;

  bool hasSelection() const { return selection.isValid() && selection.isAlive(); }
};
