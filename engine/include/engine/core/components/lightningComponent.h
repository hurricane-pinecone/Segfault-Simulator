#pragma once

#include "engine/core/rendering/mesh.h"

#include <glm/glm/ext/vector_float3.hpp>

#include <cstdint>

namespace sfs
{

/**
 * A lightning strike, at its entity's position (its TransformComponent,
 * which it needs: where it strikes the ground; an in-cloud strike flashes in
 * the cloud base above it). It flashes from when the renderer first sees it,
 * its strokes flickering over the next half second or so
 * (lightningBrightness()), then fades and is `finished`. A 3D scene's: a 2D
 * one, or one without a TransformComponent (logged), is finished at once.
 *
 * Under a physical sky it lights the scene: a cloud-to-ground strike's
 * channel is a line of light from the cloud base down to it, casting a shadow
 * near what the camera looks at, and the cloud base glows around it. The
 * channel itself is drawn, jagged and branching, under any sky. A few flash
 * at once: past four, the oldest finish early. A game creates an entity with
 * one for each strike (a storm's) and destroys it once it's finished. A strike
 * is cleared by destroying its entity: removing only this component before
 * it has finished leaves its channel's meshes until the scene ends.
 *
 * The hidden fields are the renderer's (its one writer): what a game may read
 * is `finished`.
 */
struct LightningComponent
{
  /** How bright it is (1: a typical strike; 0.5 to 2 for some variety). */
  [[sfs::range(0, 2)]] float intensity = 1.0f;
  /** In the cloud only: no channel to the ground. */
  bool inCloud = false;
  /** Shapes its channel and its strokes: give each strike its own. */
  uint32_t seed = 0;
  /**
   * Hold it this many seconds after it began (0: its first stroke's peak),
   * for screenshots and tests; below 0 it runs.
   */
  float hold = -1.0f;

  /** Seconds since it began, while the renderer animates. */
  [[sfs::hide]] float age = 0.0f;
  /** How long after it began it goes dark (lightningDuration()). */
  [[sfs::hide]] float duration = 0.0f;
  /** Whether it has gone dark (its meshes freed): the game may destroy it. */
  [[sfs::hide]] bool finished = false;
  /** Whether the renderer has shaped it: its top, width and meshes. */
  [[sfs::hide]] bool shaped = false;
  /** Where its channel leaves the cloud base (world). */
  [[sfs::hide]] glm::vec3 top{0.0f};
  /** Its channel's ribbons' width (metres; a few pixels wide however far). */
  [[sfs::hide]] float width = 1.0f;
  /** Its main channel's mesh (none for an in-cloud one). */
  [[sfs::hide]] MeshHandle channel;
  /** Its branches' mesh (none for an in-cloud one). */
  [[sfs::hide]] MeshHandle branches;
};

} // namespace sfs
