#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/camera.h"
#include "engine/runtime/rendering/space2D.h"
#include "engine/runtime/rendering/standardScene.h"
#include "engine/runtime/sceneManager/scene.h"

#include <cstddef>
#include <optional>
#include <string>

// The scene the editor edits: the engine's standard scene, in any style
// (exactly what an exported game plays), plus helpers for building it from the
// hierarchy.
class EditorScene : public sfs::StandardScene
{
public:
  using Primitive = sfs::StandardScene::BuiltinMesh;

  using sfs::StandardScene::StandardScene;

  /** The styles a new scene can start in. */
  enum class Style
  {
    ThreeD,    // ground, a cube, a light
    Flat,      // a flat 2D game's camera and a sprite
    Isometric, // an isometric 2D game's camera and a sprite
  };

  /**
   * The entity at a place in save order (see saveOrderOf()).
   * @return it, or an invalid entity past the end
   */
  sfs::Entity atSaveOrder(std::size_t index);

  /**
   * Remove every entity and, unless `keepSceneData`, the scene data (applied
   * immediately).
   *
   * @param keepSceneData keep the scene data (a prefab's tab restoring its
   *        content, which holds none, keeps its own backdrop)
   */
  void clear(bool keepSceneData = false);

  /** A named camera entity looking the way `view` does (e.g. the editor's
   * view). */
  /**
   * A game camera seeing what `view` does: posed like it, or in a 2D game
   * (given its Space2D) over the same point of the plane, orthographic, as
   * zoomed in.
   */
  sfs::Entity createCamera(const std::string& name,
                           const sfs::Camera& view,
                           const sfs::Space2D* space = nullptr);

  /** A named entity with only a transform. */
  sfs::Entity createEmpty(const std::string& name);

  /** A named entity carrying a point light. */
  sfs::Entity createPointLight(const std::string& name);

  /** A named entity drawing one of the built-in primitives. */
  sfs::Entity createPrimitive(Primitive primitive, const std::string& name);

  /** An entity with a sprite, its image still to pick. */
  sfs::Entity createSprite(const std::string& name);

  /** Destroy an entity and everything below it (applied immediately). */
  void destroyBranch(const sfs::Entity& entity);

  /** Replace the contents (and the style) with a starting layout. */
  void resetToDefault(Style style = Style::ThreeD);

  /**
   * An entity's place in save order (parents first, siblings in id order):
   * how undo and reloads keep a selection, since a reloaded scene's entities
   * come back in that order.
   * @return its index, or nothing for a destroyed or invalid entity
   */
  std::optional<std::size_t> saveOrderOf(const sfs::Entity& entity);

protected:
  void onInit() override;
};
