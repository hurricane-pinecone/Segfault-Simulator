#pragma once

#include "glm/glm/ext/quaternion_float.hpp"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * A model node's local pose as built: its TransformComponent as its model
 * instance was made (after the prefab's and scene's overrides), before anything
 * moved it. Recorded by the engine (see hierarchy::recordRestPose); not
 * reflected (never saved, shown or added by hand), as ParentComponent.
 *
 * Its fields mirror TransformComponent's, so a part turned from its rest reads
 * `transform.rotation = turn * rest.rotation`.
 */
struct RestPoseComponent
{
  /** Its position in its parent's space, as built. */
  glm::vec3 position{0.0f};
  /** Its rotation in its parent's space, as built. */
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  /** Its scale, as built. */
  glm::vec3 scale{1.0f};
};

} // namespace sfs
