#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/sceneManager/scene.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sfs
{

class PhysicsSystem3D;
class RenderSystem3D;

/**
 * The standard 3D scene that scene files describe: a RenderSystem3D drawing
 * meshes, the built-in primitive meshes (saved by name as "builtin:cube" etc.)
 * plus those the loaded game module's MeshSources build, and the game systems
 * the scene runs when it plays.
 *
 * Meshes come from the built-ins, MeshSource scripts and model assets (loaded
 * on first use through the scene's AssetStore, once it has the project's
 * AssetDatabase).
 *
 * The editor edits one of these and an exported game plays one, so a scene
 * looks and behaves the same in both. play() loads a scene file and starts
 * it; to start content that's already loaded (the editor's Play), call
 * startScripts().
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

  /**
   * The scene's physics, or null when it has none. Physics is optional: a
   * scene that wants it lists "PhysicsSystem3D" among its game systems, so it
   * exists only while the scene plays.
   */
  PhysicsSystem3D* physicsSystem();

  /** A built-in primitive mesh (registered when the scene initialises). */
  MeshHandle builtinMesh(BuiltinMesh mesh) const;

  /**
   * The game systems (System scripts, by name) this scene runs when it plays.
   * Saved with the scene.
   */
  std::vector<std::string>& gameSystems() { return m_gameSystems; }

  /**
   * Replace the scene's contents with a scene file and start its scripts and
   * game systems: what playing a scene file means, in a game or the editor.
   *
   * @return false (logged) if the file can't be loaded
   */
  bool play(const std::filesystem::path& file);

protected:
  /** Sets up rendering and the built-in meshes; overrides must call it first. */
  void onInit() override;

  /** Delivers the frame's physics contacts to game objects; overrides must call it. */
  void onUpdate(double deltaTime) override;

private:
  // Model, material and texture assets (asset:<id> names) for the scene's mesh
  // library.
  MeshLibrary::Provider modelMeshes();

  RenderSystem3D* m_render = nullptr;
  std::vector<std::string> m_gameSystems;

  MeshHandle m_cube;
  MeshHandle m_sphere;
  MeshHandle m_plane;
  MeshHandle m_cylinder;
};

} // namespace sfs
