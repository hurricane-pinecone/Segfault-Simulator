#pragma once

#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/vector_float4.hpp"

#include <string>
#include <vector>

namespace sfs
{

/**
 * Draws a mesh at the entity's TransformComponent. Entities sharing a mesh
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
 * @param bool emitsLight - whether its materials' emission lights what's
 *                          around it (see emissionLights.h), default true
 * @param float lightStrength - how brightly its emission lights things
 *                              (1 = as bright as it glows)
 * @param float lightRange - how far its emission lights things (1 = as far
 *                           as that much glow would reach)
 * @param bool lightCastsShadows - whether things block its emission's light
 *                                 (as a point light's; see
 *                                 PointLightComponent), default true
 * @param glm::vec3 emissionColor - the colour of its own glow (linear RGB)
 * @param float emission - how bright it glows over its materials' own
 *                         emission, all over it (0: not at all, the default;
 *                         above 1 blooms): a lamp switched on, set from a
 *                         script. It lights nothing around it: give a lamp a
 *                         PointLightComponent for that
 *
 * RenderSystem fills in `bounds` at the start of each of its updates, for
 * systems that need the mesh's size or a world box around it
 * (`bounds.transformed(hierarchy::worldMatrix(entity))`).
 */
struct MeshRendererComponent
{
  MeshHandle mesh;
  [[sfs::materials]] std::vector<std::string> materials;
  [[sfs::color]] glm::vec4 tint{1.0f};
  bool castsShadow = true;
  [[sfs::emissionLight]] bool emitsLight = true;
  [[sfs::emissionLight, sfs::range(0, 4)]] float lightStrength = 1.0f;
  [[sfs::emissionLight, sfs::range(0, 4)]] float lightRange = 1.0f;
  [[sfs::emissionLight]] bool lightCastsShadows = true;
  [[sfs::color]] glm::vec3 emissionColor{1.0f};
  [[sfs::range(0, 50)]] float emission = 0.0f;

  /**
   * The mesh's bounds in its own space (a skinned mesh's as bound), set by
   * RenderSystem's update once the mesh is in its library. Empty until then,
   * and for a handle the library doesn't know: treat empty as still loading.
   * A mesh assigned during a frame gets its bounds at the next update.
   */
  [[sfs::hide]] Aabb bounds;
  /** The mesh `bounds` were taken from (RenderSystem's). */
  [[sfs::hide]] MeshHandle boundsOf;

  MeshRendererComponent() = default;

  explicit MeshRendererComponent(MeshHandle mesh,
                                 glm::vec4 tint = glm::vec4(1.0f),
                                 bool castsShadow = true)
      : mesh(mesh), materials(), tint(tint), castsShadow(castsShadow)
  {
  }
};

} // namespace sfs
