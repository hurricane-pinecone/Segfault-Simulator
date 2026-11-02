#pragma once

#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>
#include <vector>

namespace sfs
{

/**
 * A lightning strike, as a game sets one off (RenderSystem::
 * strikeLightning()); a 3D scene's. Under a physical sky it lights the scene:
 * a cloud-to-ground strike's channel is a very bright line of light from the
 * cloud base down to `position` (falling off with distance as a line light
 * does, specular on wet ground, with a shadow when it's near what the camera
 * looks at), and the cloud base glows around it, lighting the land a little
 * further off and flashing in the sky and its reflections. The channel itself
 * is drawn, jagged and branching, glowing (bloom picks it up), as its strokes
 * flicker (lightningBrightness()), with a short afterimage. An in-cloud
 * strike only lights the clouds (and, through them, the land).
 *
 * @param position  where it strikes the ground (world); an in-cloud strike
 *                  flashes in the cloud base above it
 * @param intensity how bright it is (1: a typical strike; 0.5 to 2 for some
 *                  variety)
 * @param inCloud   in the cloud only (no channel to the ground)
 * @param seed      shapes its channel and its strokes (each strike's own)
 */
struct LightningStrike
{
  glm::vec3 position{0.0f};
  float intensity = 1.0f;
  bool inCloud = false;
  uint32_t seed = 0;

  bool operator==(const LightningStrike&) const = default;
};

/**
 * How bright a strike is `seconds` after it began, 0 to about 1: its strokes
 * (a cloud-to-ground strike's 2 to 5 return strokes over 0.15 to 0.5 s, each
 * a sharp flash dying off in tens of milliseconds, one of them sometimes held
 * by a continuing current; an in-cloud strike's softer, flickering pulses),
 * then nothing. The light it gives is this x its intensity.
 */
float lightningBrightness(const LightningStrike& strike, float seconds);

/**
 * How bright its drawn channel is: lightningBrightness(), and after each
 * stroke a short afterimage, fading.
 */
float lightningChannelBrightness(const LightningStrike& strike, float seconds);

/** How long after it began a strike has gone dark (its afterimage too). */
float lightningDuration(const LightningStrike& strike);

/** One straight piece of a strike's channel (or of a branch off it). */
struct LightningSegment
{
  glm::vec3 from{0.0f};
  glm::vec3 to{0.0f};
  float width = 1.0f;      // x the channel's width
  float brightness = 1.0f; // x the channel's
  bool branch = false;     // a branch (lit only by the first stroke)
};

/**
 * A cloud-to-ground strike's channel, from `top` (in the cloud base) down to
 * its position: jagged (displaced again and again at finer scales), with
 * branches forking off its upper part (some with branches of their own) that
 * die out before the ground. The same strike always gives the same channel.
 */
std::vector<LightningSegment> lightningChannel(const LightningStrike& strike,
                                               const glm::vec3& top);

/**
 * Where a cloud-to-ground strike's channel leaves the cloud base `cloudBase`
 * (world Z) above it: a little to one side (channels slant).
 */
glm::vec3 lightningTop(const LightningStrike& strike, float cloudBase);

/**
 * A channel's segments as a mesh to draw (additively, unlit: its vertex
 * colour is its light): each a ribbon `width` wide (x the segment's) facing
 * `eye`, bright along its middle and fading to its edges, the channel's
 * branches or not (`branches`). Empty if there are none.
 */
MeshData lightningMesh(const std::vector<LightningSegment>& segments,
                       const glm::vec3& eye,
                       float width,
                       bool branches);

/**
 * How bright a channel from `low` to `high` lights `point`, for a channel
 * whose light is 1 per metre: the inverse square summed along it (about 1 /
 * the distance near it, 1 / its square far off). And where along it the
 * light comes from on average, seen from `point` (its shadows' and its
 * direction's).
 */
float lightningLineLight(const glm::vec3& low,
                         const glm::vec3& high,
                         const glm::vec3& point);
glm::vec3 lightningLightCentre(const glm::vec3& low,
                               const glm::vec3& high,
                               const glm::vec3& point);

} // namespace sfs
