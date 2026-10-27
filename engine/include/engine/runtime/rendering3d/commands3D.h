#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "engine/runtime/rendering3d/frame3D.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace sfs
{

/**
 * Coarse draw order for 3D commands. Opaque geometry draws first (sorted to
 * minimise state changes); transparent geometry will draw after it, back to
 * front.
 */
enum class RenderPass3D : uint8_t
{
  Opaque = 0,
  Transparent,
};

/** Every instance of one mesh this frame, split into seen and shadow-casting. */
struct MeshDrawCommand
{
  RenderPass3D pass = RenderPass3D::Opaque;
  MeshHandle mesh;
  std::vector<MeshInstance> instances;
  std::vector<MeshInstance> shadowInstances;
};

/** The command set 3D render modules emit (see RenderCommandTraits). */
using AnyRenderCommand3D = std::variant<MeshDrawCommand>;

} // namespace sfs
