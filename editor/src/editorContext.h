#pragma once

#include "project/project.h"

#include "engine/core/ecs/entity.h"

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

  // The selected entity; invalid (default) when nothing is selected.
  sfs::Entity selection;

  bool hasSelection() const { return selection.isValid() && selection.isAlive(); }
};
