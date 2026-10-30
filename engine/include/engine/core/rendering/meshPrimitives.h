#pragma once

#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs::meshes
{

// Primitive builders. Every shape is centred on the origin in X/Y and sits on
// the Z = 0 plane (its base at Z = 0), so placing an entity at a ground point
// stands the shape on it. Faces are flat-shaded (vertices are not shared across
// hard edges), which suits low-poly art; curved sides use smooth normals.

/** An axis-aligned box of the given full size. */
MeshData box(const glm::vec3& size, const glm::vec3& color = glm::vec3(1.0f));

/** A single upward-facing rectangle at Z = 0. */
MeshData plane(const glm::vec2& size, const glm::vec3& color = glm::vec3(1.0f));

/** A capped cylinder around +Z. segments is clamped to at least 3. */
MeshData cylinder(float radius,
                  float height,
                  int segments,
                  const glm::vec3& color = glm::vec3(1.0f));

/** A capped cone around +Z, apex at Z = height. segments >= 3. */
MeshData cone(float radius,
              float height,
              int segments,
              const glm::vec3& color = glm::vec3(1.0f));

/**
 * A UV sphere of the given radius, resting on Z = 0 (centre at Z = radius).
 * segments (around) >= 3, rings (pole to pole) >= 2.
 */
MeshData sphere(float radius,
                int segments,
                int rings,
                const glm::vec3& color = glm::vec3(1.0f));

/**
 * A triangular prism (a gable roof): a ridge along X, `size.y` wide at the
 * base and `size.z` tall, sloping down to both long sides.
 */
MeshData prism(const glm::vec3& size, const glm::vec3& color = glm::vec3(1.0f));

/**
 * Append `source` to `target` with every vertex transformed. Normals use the
 * inverse-transpose, so non-uniform scales stay lit correctly. Use it to build
 * a compound model (a barn = box + prism) into a single mesh, and so one draw.
 */
void append(MeshData& target,
            const MeshData& source,
            const glm::mat4& transform = glm::mat4(1.0f));

} // namespace sfs::meshes
