#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "engine/runtime/sceneManager/scene.h"

#include <string>
#include <vector>

namespace sfs
{

class RenderSystem3D;

/**
 * The standard 3D scene that scene files describe: a RenderSystem3D drawing
 * meshes, the built-in primitive meshes (saved by name as "builtin:cube" etc.)
 * plus those the loaded game module's MeshSources build, and the game systems
 * the scene runs when it plays.
 *
 * The editor edits one of these and an exported game plays one, so a scene
 * looks and behaves the same in both. Load content into it with
 * SceneSerializer (passing gameSystems()), then call startScripts().
 */
class Scene3D : public Scene
{
public:
  enum class BuiltinMesh
  {
    Cube,
    Sphere,
    Plane,
    Cylinder,
  };

  using Scene::Scene;

  RenderSystem3D& renderSystem() { return *m_render; }

  /** A built-in primitive mesh (registered when the scene initialises). */
  MeshHandle builtinMesh(BuiltinMesh mesh) const;

  /**
   * The game systems (System scripts, by name) this scene runs when it plays.
   * Saved with the scene.
   */
  std::vector<std::string>& gameSystems() { return m_gameSystems; }

protected:
  /** Sets up rendering and the built-in meshes; overrides must call it first. */
  void onInit() override;

private:
  RenderSystem3D* m_render = nullptr;
  std::vector<std::string> m_gameSystems;

  MeshHandle m_cube;
  MeshHandle m_sphere;
  MeshHandle m_plane;
  MeshHandle m_cylinder;
};

} // namespace sfs
