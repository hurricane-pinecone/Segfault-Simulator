#pragma once

#include "project/project.h"

#include "engine/core/ecs/entity.h"

#include <optional>

namespace sfs
{
class Scene;
class RenderSystem3D;
} // namespace sfs

class EditorScene;

// The editor state every panel reads and edits: the open project, the scene
// being edited, and the current selection.
struct EditorContext
{
  std::optional<Project> project;

  EditorScene* scene = nullptr;
  sfs::RenderSystem3D* render = nullptr;

  // The selected entity; invalid (default) when nothing is selected.
  sfs::Entity selection;

  bool hasSelection() const { return selection.isValid() && selection.isAlive(); }
};
