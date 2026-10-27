#pragma once

#include "engine/core/rendering/3d/aabb.h"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>
#include <vector>

namespace sfs
{

/**
 * One vertex of a 3D mesh. Colour is per-vertex albedo (linear RGB), so
 * untextured low-poly models carry their palette in the mesh itself; an
 * instance tint multiplies it at draw time.
 */
struct MeshVertex
{
  glm::vec3 position{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  glm::vec3 color{1.0f};
};

/**
 * CPU-side mesh: an indexed triangle list (counter-clockwise front faces,
 * Z up). Plain data, so it can be built, combined and tested without a GPU;
 * a renderer uploads it once and hands back a MeshHandle.
 */
struct MeshData
{
  std::vector<MeshVertex> vertices;
  std::vector<uint32_t> indices;

  bool empty() const { return indices.empty(); }

  /** Bounds of every vertex position. */
  Aabb bounds() const;
};

/**
 * Opaque reference to a mesh a renderer has uploaded. Value 0 is the null
 * handle; renderers never issue it.
 */
struct MeshHandle
{
  uint32_t id = 0;

  bool valid() const { return id != 0; }

  bool operator==(const MeshHandle&) const = default;
};

} // namespace sfs
