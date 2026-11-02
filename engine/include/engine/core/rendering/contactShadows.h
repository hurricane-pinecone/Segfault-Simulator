#pragma once

#include "nlohmann/json.hpp"

namespace sfs
{

/**
 * Screen-space contact shadows: the small, sharp shadows where things meet
 * the ground and in fine crevices (a fence post's foot, under a crate or a
 * wheel, around someone's feet), finer than the sun's shadow maps can
 * hold. From each point a short ray is walked toward the frame's light (the
 * sun, or the moon while it lights the scene) over the depth of what's
 * drawn; something in front of the ray, but no thicker than `thickness`,
 * shades it. Worked out with ambient occlusion's passes (at half the
 * screen's resolution, from the same depth), and taken only from the sun's
 * light, never the sky's, the ambient level or point lights'. A physical
 * sky's lighting only, and a 3D scene's.
 *
 * Grass blades don't cast them (they'd darken whole fields); the ground
 * under them takes them from what stands on it.
 *
 * @param enabled   whether to at all (off by default: costs nothing)
 * @param length    how far toward the light the ray goes, world units (0.5:
 *                  the foot of posts and crates; longer reaches farther
 *                  from what casts them)
 * @param thickness how thick what's drawn is taken to be behind its front,
 *                  world units: thin things (posts) cast only as far as it
 *                  allows, and things this much in front of the ray don't
 * @param strength  how dark they get (0: none; 1 none of the sun's light)
 */
struct ContactShadowSettings
{
  bool enabled = false;
  float length = 0.5f;
  float thickness = 0.3f;
  float strength = 0.8f;

  nlohmann::json toJson() const;
  /** Missing keys keep `defaults`' values. */
  static ContactShadowSettings fromJson(const nlohmann::json& json,
                                        const ContactShadowSettings& defaults);
  static ContactShadowSettings fromJson(const nlohmann::json& json);

  bool operator==(const ContactShadowSettings&) const = default;
};

} // namespace sfs
