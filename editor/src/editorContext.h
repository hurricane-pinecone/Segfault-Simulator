#pragma once

#include "project/project.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/vector_int4.hpp"

#include <cstdint>
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

  // While playing: the game gets input (the viewport has focus).
  bool gameHasInput = false;

  // Where the viewport shows the scene, so the game's mouse positions can be
  // made relative to it: the OS window it's in (SDL window id; the panel may
  // be dragged out into its own) and the image's rectangle in that window, in
  // window pixels (x, y, width, height).
  uint32_t viewportWindowId = 0;
  glm::ivec4 viewportRect{0};

  // The selected entity; invalid (default) when nothing is selected.
  sfs::Entity selection;

  // The project's asset catalogue (its assets/ folder); null without a project.
  sfs::AssetDatabase* assets = nullptr;

  // The asset selected in the Assets panel (invalid when none). The Inspector
  // shows it while no entity is selected; selecting an entity clears it.
  sfs::AssetId selectedAsset;

  bool hasSelection() const { return selection.isValid() && selection.isAlive(); }
};
