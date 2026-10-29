#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * Flattens grass (GrassComponent) it moves through, as a sphere: for things
 * without a rigid body (a character moved by script, a cart on rails, a
 * tractor's wheels). Rigid bodies flatten grass by themselves
 * (RigidBody3DComponent::bendsGrass).
 *
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
  bool fitMesh = true;
  [[sfs::range(0.05, 20)]] [[sfs::showIf(!fitMesh)]] float radius = 0.5f;
  glm::vec3 offset{0.0f};
};

} // namespace sfs
