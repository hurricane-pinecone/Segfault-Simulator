#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "glm/glm/ext/vector_float4.hpp"

namespace sfs
{

/**
 * Draws a mesh at the entity's Transform3DComponent. Entities sharing a mesh
 * are batched into one instanced draw, so a field of identical crops costs
 * one draw call however many there are.
 *
 * @param MeshHandle mesh - the uploaded mesh (see MeshLibrary)
 * @param glm::vec4 tint - multiplies the mesh's vertex colours, default white
 * @param bool castsShadow - whether it occludes the sun, default true
 */
struct MeshRendererComponent
{
  MeshHandle mesh;
  [[sfs::color]] glm::vec4 tint{1.0f};
  bool castsShadow = true;

  MeshRendererComponent() = default;

  explicit MeshRendererComponent(MeshHandle mesh,
                                 glm::vec4 tint = glm::vec4(1.0f),
                                 bool castsShadow = true)
      : mesh(mesh), tint(tint), castsShadow(castsShadow)
  {
  }
};

} // namespace sfs
