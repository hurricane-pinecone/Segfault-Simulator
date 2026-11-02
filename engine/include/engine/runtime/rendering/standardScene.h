#pragma once

#include "engine/core/rendering/mesh.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

class PhysicsSystem3D;
class RenderSystem;

/**
 * The standard scene that scene files describe, in any style (flat, isometric
 * or 3D: the file's render settings say which): a RenderSystem drawing
 * meshes, sprites, particles, decals, grass and UI; the built-in primitive
 * meshes (saved by name as "builtin:cube" etc.) plus those the loaded game
 * module's MeshSources build; and the game systems the scene runs when it
 * plays.
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
class StandardScene : public Scene
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

  RenderSystem& renderSystem() { return *m_render; }

  /**
   * A serializer for this scene: meshes by the scene's library, prefabs
   * ("asset:<id>") from its asset store.
   */
  SceneSerializer serializer();

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

  /**
   * Make an instance of a prefab ("asset:<id>") while the game runs, under
   * `parent` (none: a root), and start its scripts: each entity's game object
   * is made and its onCreate run, as when a scene starts.
   *
   * @return its root, or a placeholder (logged) if the prefab can't be read
   */
  Entity instantiate(std::string_view prefab, const Entity& parent = {});

protected:
  /** Sets up rendering and the built-in meshes; overrides must call it first.
   */
  void onInit() override;
  // Its UI (UiModule) takes clicks on it first.
  void filterInput(Input& input) override;

  /** Swaps in models loaded in the background; overrides must call it. */
  void onUpdate(double deltaTime) override;

private:
  // Model, material and texture assets (asset:<id> names) for the scene's mesh
  // library.
  MeshLibrary::Provider modelMeshes();

  RenderSystem* m_render = nullptr;
  std::vector<std::string> m_gameSystems;
  uint64_t m_seenLoads = 0; // the asset store's loadGeneration() last seen

  MeshHandle m_cube;
  MeshHandle m_sphere;
  MeshHandle m_plane;
  MeshHandle m_cylinder;
};

} // namespace sfs
