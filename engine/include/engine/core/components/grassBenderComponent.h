#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * Flattens grass (GrassComponent) it moves through, as a sphere: anything
 * that should (a character, a crate knocked about, a tractor's wheels). The
 * one way grass is bent: nothing else (a rigid body, a collider) bends it.
 *
 * @param bool fitColliders - a sphere for each of the entity's own solid
 *                            colliders (a sphere as itself; a box or capsule
 *                            as a sphere as wide as its footprint, its bottom
 *                            where theirs is): a physics prop. Takes
 *                            precedence over fitMesh
 * @param bool fitMesh - the sphere around the entity's own mesh (its
 *                       MeshRendererComponent's), as it's placed: put one on
 *                       each wheel and only the wheels bend grass. Without a
 *                       mesh (or with this off), `radius` sizes it
 * @param float radius - how much grass around it bends (a scaled entity scales
 *                       it by its largest scale axis)
 * @param glm::vec3 offset - its centre relative to where it'd be (the mesh's
 *                           middle, or the entity's origin)
 */
struct GrassBenderComponent
{
  bool fitColliders = false;
  [[sfs::showIf(!fitColliders)]] bool fitMesh = true;
  [[sfs::range(0.05, 20)]] [[sfs::showIf(!fitMesh)]] float radius = 0.5f;
  glm::vec3 offset{0.0f};
};

} // namespace sfs
