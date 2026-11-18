#pragma once

#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/camera.h"
#include "engine/core/rendering/mesh.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/picking.h"
#include "engine/runtime/sceneManager/scene.h"

#include <glm/glm/ext/matrix_float4x4.hpp>
#include <glm/glm/ext/vector_float3.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sfs
{

class RenderSystem;

namespace facility
{

/**
 * The scene's meshes, as game code uses it (`meshes()` on a System, Service,
 * GameObject or Scene): finding them by name, their bounds and triangles,
 * what a ray meets as they're drawn (picking), meshes and materials made as
 * the game runs. A value handle over the
 * scene's MeshLibrary, valid for the step that got it: get it where it's
 * used and keep it in no member. What points into the library (bounds(),
 * positions()) is valid for this step only. In a scene that doesn't render
 * its commands do nothing and its queries give empty answers, with one
 * warning a run (available() tells).
 */
class Meshes
{
public:
  /**
   * Uploads a mesh, optionally under a name: a name is how saved scenes
   * refer to it.
   * @param mesh the mesh
   * @param name its name (empty: none)
   * @return its handle; invalid (logged) if it's empty, the name is taken,
   *         the upload failed, or the scene doesn't render
   */
  MeshHandle add(const MeshData& mesh, std::string name = {}) const;

  /** @return whether the scene renders behind this facade */
  inline bool available() const { return m_render != nullptr; }

  /**
   * A mesh's local bounds, valid for this step only.
   * @param mesh the mesh
   * @return its bounds; null if it isn't the scene's (or is a stand-in)
   */
  const Aabb* bounds(MeshHandle mesh) const;

  /**
   * The mesh with a name.
   * @param name its name
   * @return its handle; invalid if there's none
   */
  MeshHandle find(std::string_view name) const;

  /**
   * The meshes of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade, absent when it has no RenderSystem
   */
  static Meshes in(Registry* registry);

  /**
   * A mesh's name.
   * @param mesh the mesh
   * @return its name; empty if it has none or isn't the scene's
   */
  std::string_view nameOf(MeshHandle mesh) const;

  /**
   * The entity under a ray (a click through cameras().screenRay()): the
   * nearest hit, a mesh where the ray meets its triangles, anything else
   * where it enters its bounds; of several at one distance, the smallest
   * (pickEntity() in picking.h).
   * @param ray the ray
   * @return the hit; none if it hits nothing, or the scene doesn't render
   */
  std::optional<PickHit> pickEntity(const Ray& ray) const;

  /**
   * The mesh surface under a ray: the nearest triangle of any entity's mesh,
   * and the section (material slot) it's in (pickSurface() in picking.h).
   * @param ray the ray
   * @return the hit; none if it hits nothing, or the scene doesn't render
   */
  std::optional<SurfaceHit> pickSurface(const Ray& ray) const;

  /**
   * A mesh's vertex positions, in its own space, valid for this step only.
   * @param mesh the mesh
   * @return its positions; empty if it isn't the scene's
   */
  std::span<const glm::vec3> positions(MeshHandle mesh) const;

  /**
   * The nearest of a mesh's triangles a ray hits, the mesh placed by
   * `world`.
   * @param mesh the mesh
   * @param world where the mesh is
   * @param ray the ray
   * @return the hit; none if it misses or the mesh isn't the scene's
   */
  std::optional<MeshLibrary::RayHit>
  raycast(MeshHandle mesh, const glm::mat4& world, const Ray& ray) const;

  /**
   * Frees a mesh the scene added; other handles are ignored.
   * @param mesh the mesh
   */
  void remove(MeshHandle mesh) const;

  /**
   * The mesh with a name, loading it if the scene has none by that name yet.
   * @param name its name
   * @return its handle; invalid if it can't be found
   */
  MeshHandle resolve(std::string_view name) const;

  /**
   * Makes or changes a named material (one whose texture the game paints):
   * everything using it shows the change.
   * @param name its name
   * @param material what it is
   * @return its handle; invalid when the scene doesn't render
   */
  MaterialHandle setMaterial(std::string_view name,
                             const MeshMaterial& material) const;

private:
  inline Meshes(RenderSystem* render, Registry* registry)
      : m_render(render), m_registry(registry)
  {
  }

  // The library; null (and warned about once) when the scene doesn't render.
  MeshLibrary* backend() const;

  // The RenderSystem whose library it is and which draws the sprites a pick
  // tries, and the registry whose entities a pick tries.
  RenderSystem* m_render = nullptr;
  Registry* m_registry = nullptr;
};

} // namespace facility

inline facility::Meshes System::meshes() const
{
  return facility::Meshes::in(registry);
}

inline facility::Meshes Service::meshes() const
{
  return facility::Meshes::in(m_registry);
}

inline facility::Meshes GameObject::meshes() const
{
  return m_scene ? m_scene->meshes() : facility::Meshes::in(nullptr);
}

inline facility::Meshes Scene::meshes()
{
  return facility::Meshes::in(&m_registry);
}

} // namespace sfs
