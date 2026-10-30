#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * A point light carried by an entity with a TransformComponent, in any render
 * style. All units are world units: in a flat scene, pixels; in an isometric
 * one, tiles across and elevation levels up (so offset.z is the lamp's height
 * above the ground it stands on); in a 3D scene, metres.
 *
 * @param glm::vec3 color - linear RGB tint, default warm lamp
 * @param float intensity - brightness multiplier (0 = off), default 1
 * @param float radius - reach in world units, default 8
 * @param glm::vec3 offset - emitter position relative to the transform, e.g.
 *                           a lantern's flame above its base, default origin
 * @param bool castsShadow - whether things block its light, default true (the
 *                           lights nearest the view, up to
 *                           ShadowSettings::pointLights, cast them)
 * @param float sourceRadius - how big the lamp is (its bulb, its flame):
 *                             shiny things reflect a highlight that big,
 *                             default 0.05
 */
struct PointLightComponent
{
  [[sfs::color]] glm::vec3 color{1.0f, 0.85f, 0.6f};
  [[sfs::range(0, 10)]] float intensity = 1.0f;
  [[sfs::range(0, 100)]] float radius = 8.0f;
  glm::vec3 offset{0.0f};
  bool castsShadow = true;
  [[sfs::range(0, 5)]] float sourceRadius = 0.05f;

  PointLightComponent() = default;

  PointLightComponent(glm::vec3 color,
                      float intensity,
                      float radius,
                      glm::vec3 offset = glm::vec3(0.0f))
      : color(color), intensity(intensity), radius(radius), offset(offset)
  {
  }
};

} // namespace sfs
