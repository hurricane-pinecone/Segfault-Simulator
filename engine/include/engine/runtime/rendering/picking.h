#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/camera.h"

#include <cstddef>
#include <optional>

namespace sfs
{

class MeshLibrary;
class MouseInput;
class RenderSystem;
class Scene;
struct CursorPickComponent;

/** Half-size of the box that stands in for an entity with no mesh (a light, an
 * empty). */
inline constexpr float kPickMarkerRadius = 0.3f;

/**
 * An entity's world-space bounds: its mesh's bounds through its world matrix
 * (parents included), or a small marker box at its position if it has no mesh.
 * Given the render system that draws it, also its sprite where that was drawn
 * last frame, and in a 2D game (a Space2D) its position on the game's plane.
 *
 * @return nothing if the entity has no TransformComponent.
 */
std::optional<Aabb> entityBounds(const Entity& entity,
                                 const MeshLibrary& meshes,
                                 const RenderSystem* drawnBy = nullptr);

/** What a pick found. */
struct PickHit
{
  /** The entity picked. */
  Entity entity;
  /** How far along the ray it was hit. */
  float distance = 0.0f;
  /**
   * The entity whose mesh or bounds the ray hit: `entity` itself, or with
   * pickPickable() a mesh at or below it.
   */
  Entity part;
  /** Where the ray hit, in world space. */
  glm::vec3 point{0.0f};
};

/**
 * The entity under a ray (e.g. a mouse click through Camera::screenRay): the
 * nearest hit. A mesh is hit where the ray meets its triangles (its bounds
 * only rule it out), so a camera inside a landscape's bounds still picks what
 * stands on it; anything else where the ray enters its bounds
 * (entityBounds()). When several are hit at the same distance, the smallest
 * wins, so an object standing on a large floor is picked over the floor. It
 * sees every living entity, those not yet flushed into queries included.
 */
std::optional<PickHit> pickEntity(Scene& scene,
                                  const MeshLibrary& meshes,
                                  const Ray& ray,
                                  const RenderSystem* drawnBy = nullptr);

/**
 * The pickable entity under a ray (PickableComponent): of the meshes with an
 * enabled pickable entity at or above them, the one whose triangles the ray
 * meets nearest, as that pickable entity. Each mesh is ruled out first by its
 * bounds (as posed). Meshes with no pickable entity above them, or under a
 * disabled one, are skipped: they don't block the ray.
 *
 * @param scene the scene whose meshes it tries
 * @param meshes the library their meshes are in
 * @param ray the ray, e.g. RenderSystem::screenRay at the cursor
 * @return the hit (entity: the pickable one, part: the mesh's entity), or
 *         nothing
 */
std::optional<PickHit>
pickPickable(Scene& scene, const MeshLibrary& meshes, const Ray& ray);

/**
 * Fills a CursorPickComponent for a frame: its ray, whether the UI has the
 * mouse and whether the cursor is in view, and, when the mouse isn't blocked
 * and a button was pressed this frame (every frame with `hover`), what
 * pickPickable() finds under the cursor. What was picked is cleared first.
 * StandardScene calls it for the scene camera before input reaches objects.
 *
 * @param scene the scene to pick in
 * @param meshes the library its meshes are in
 * @param ray the ray through the cursor
 * @param mouse this frame's mouse, after the UI has taken it or not
 * @param pick the component to fill
 */
void pickUnderCursor(Scene& scene,
                     const MeshLibrary& meshes,
                     const Ray& ray,
                     const MouseInput& mouse,
                     CursorPickComponent& pick);

struct SurfaceHit
{
  Entity entity;
  std::size_t section = 0; // the mesh section (material slot) hit
  float distance = 0.0f;
};

/**
 * The mesh surface under a ray: the nearest triangle of any entity's mesh,
 * exact where pickEntity uses bounds. For dropping things onto objects, e.g. a
 * material onto the part of a model under the cursor. Like pickEntity(), it
 * sees entities not yet flushed into queries.
 */
std::optional<SurfaceHit>
pickSurface(Scene& scene, const MeshLibrary& meshes, const Ray& ray);

} // namespace sfs
