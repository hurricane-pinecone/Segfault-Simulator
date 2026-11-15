#pragma once

#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/matrix_float4x3.hpp"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>

namespace sfs
{

/**
 * An entity's world matrix as the transform pass (TransformSystem) last made
 * it, once a frame, at the start of the render phase. The pass keeps one on
 * every entity with a TransformComponent; it's not reflected (never saved,
 * shown or added by hand).
 *
 * Read it through hierarchy::cachedWorldMatrix() in code that runs after the
 * pass in the same frame (the render phase). Anywhere else, read
 * hierarchy::worldMatrix(), which is always fresh and uses this cache for the
 * levels that haven't changed since the pass.
 *
 * 64 bytes: the matrix, then what tells whether it's still current.
 */
struct WorldTransformComponent
{
  /**
   * The local-to-world matrix, affine: the scaled x, y and z axes, then the
   * origin (as hierarchy::composeWorld() makes it).
   */
  glm::mat4x3 world{1.0f};
  /** hierarchy::transformHash() of the TransformComponent it was made from. */
  uint64_t localHash = 0;
  /**
   * The number of the transform pass that last changed `world`, so a reader
   * can tell what changed by comparing it; 0 while it isn't made yet (read
   * the entity through hierarchy::worldMatrix() then).
   */
  uint32_t version = 0;
  /**
   * The nearest ancestor with a TransformComponent when it was made (its
   * parent, unless that only groups); HierarchyLinks::kNone for none.
   */
  Entity::EntityId parent = Entity::InvalidId;

  /** The world matrix as a 4x4 (bottom row 0, 0, 0, 1). */
  glm::mat4 matrix() const { return glm::mat4(world); }
  /** The entity's world position (the matrix's origin). */
  glm::vec3 position() const { return world[3]; }
};

/**
 * Tag: the SFS_CHECK_TRANSFORMS debug check has found this entity's cached
 * world matrix wrong once, and said so; it doesn't say so again. Not
 * reflected.
 */
struct TransformCheckWarned
{
};

} // namespace sfs
