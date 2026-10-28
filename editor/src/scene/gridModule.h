#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering3d/commands3D.h"
#include "engine/runtime/rendering3d/renderContext3D.h"

namespace sfs
{
class MeshLibrary;
}

// A floor grid for looking at something on its own (a prefab), like a
// modelling tool's: metre lines, every tenth stronger, fading with distance,
// and the X (red) and Y (green) axes. Drawn by the render system, so things
// hide it and cast shadows on it, but not part of the scene: nothing saves,
// picks or lists it.
class GridModule : public sfs::CommandModule<sfs::RenderContext3D, sfs::MeshDrawCommand>
{
public:
  /** Upload the grid's mesh into `meshes` (the scene's). */
  explicit GridModule(sfs::MeshLibrary& meshes);

  void computeCommands(const sfs::RenderContext3D& context) override;

private:
  sfs::MeshHandle m_mesh;
};
