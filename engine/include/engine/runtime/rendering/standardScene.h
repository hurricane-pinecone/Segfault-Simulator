#pragma once

#include "engine/core/rendering/mesh.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/scripting/sceneScripts.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <cstdint>
#include <filesystem>

namespace sfs
{

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
 * startScripts(). Its PrefabSystem makes prefab copies while it runs.
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

  /** A built-in primitive mesh (registered when the scene initialises). */
  MeshHandle builtinMesh(BuiltinMesh mesh) const;

  /**
   * Replace the scene's contents with a scene file and start its scripts,
   * game systems and services: what playing a scene file means, in a game or
   * the editor.
   *
   * @return false (logged) if the file can't be loaded
   */
  bool play(const std::filesystem::path& file);

  /**
   * The scene's render system, for its host (the editor, the game loop):
   * game code uses the facilities (graphics(), meshes(), cameras(), ...).
   */
  RenderSystem& renderSystem() { return *m_render; }

  /**
   * The game systems and services (System and Service scripts, by name) this
   * scene runs when it plays. Saved with the scene.
   * @return them, to read or change
   */
  SceneScripts& sceneScripts() { return m_scripts; }

  /**
   * A serializer for this scene: meshes by the scene's library, prefabs
   * ("asset:<id>") from its asset store.
   */
  SceneSerializer serializer();

protected:
  // Its UI (UiModule) takes clicks on it first.
  void filterInput(Input& input) override;

  /** Sets up rendering and the built-in meshes; overrides must call it first.
   */
  void onInit() override;

  /** Swaps in models loaded in the background; overrides must call it. */
  void onUpdate(double deltaTime) override;

private:
  // Model, material and texture assets (asset:<id> names) for the scene's mesh
  // library.
  MeshLibrary::Provider modelMeshes();

  RenderSystem* m_render = nullptr;
  SceneScripts m_scripts;
  uint64_t m_seenLoads = 0; // the asset store's loadGeneration() last seen

  MeshHandle m_cube;
  MeshHandle m_sphere;
  MeshHandle m_plane;
  MeshHandle m_cylinder;
};

} // namespace sfs
