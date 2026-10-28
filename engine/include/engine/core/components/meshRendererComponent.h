#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "glm/glm/ext/vector_float4.hpp"

#include <string>
#include <vector>

namespace sfs
{

/**
 * Draws a mesh at the entity's Transform3DComponent. Entities sharing a mesh
 * are batched into one instanced draw, so a field of identical crops costs
 * one draw call however many there are.
 *
 * @param MeshHandle mesh - the uploaded mesh (see MeshLibrary)
 * @param std::vector<std::string> materials - the material for each of the
 *        mesh's sections (its material slots), in order, by name ("asset:<id>"
 *        of a material asset); empty, or past the end, for the section's own
 * @param glm::vec4 tint - multiplies the mesh's colours, default white; alpha
 *                         below 1 makes it see-through
 * @param bool castsShadow - whether it occludes the sun, default true
 */
struct MeshRendererComponent
{
  MeshHandle mesh;
  [[sfs::materials]] std::vector<std::string> materials;
  [[sfs::color]] glm::vec4 tint{1.0f};
  bool castsShadow = true;

  MeshRendererComponent() = default;

  explicit MeshRendererComponent(MeshHandle mesh,
                                 glm::vec4 tint = glm::vec4(1.0f),
                                 bool castsShadow = true)
      : mesh(mesh), materials(), tint(tint), castsShadow(castsShadow)
  {
  }
};

} // namespace sfs
