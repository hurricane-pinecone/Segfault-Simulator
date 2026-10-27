#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "engine/runtime/sceneManager/scene.h"

#include <string>

namespace sfs
{
class RenderSystem3D;
}

// The scene the editor edits: a 3D scene (RenderSystem3D + MeshModule) plus the
// editor's built-in primitive meshes, so entities can be created from the
// hierarchy before a project has assets of its own.
class EditorScene : public sfs::Scene
{
public:
  enum class Primitive
  {
    Cube,
    Sphere,
    Plane,
    Cylinder,
  };

  using sfs::Scene::Scene;

  sfs::RenderSystem3D& renderSystem() { return *m_render; }

  /** A named entity with only a transform. */
  sfs::Entity createEmpty(const std::string& name);

  /** A named entity drawing one of the built-in primitives. */
  sfs::Entity createPrimitive(Primitive primitive, const std::string& name);

  /** A named entity carrying a point light. */
  sfs::Entity createPointLight(const std::string& name);

protected:
  void onInit() override;

private:
  // A starting layout (ground, a cube, a light) until scenes load from files.
  void populateDefault();

  sfs::RenderSystem3D* m_render = nullptr;

  sfs::MeshHandle m_cube;
  sfs::MeshHandle m_sphere;
  sfs::MeshHandle m_plane;
  sfs::MeshHandle m_cylinder;
};
