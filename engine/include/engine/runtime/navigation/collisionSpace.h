#pragma once

#include "engine/runtime/navigation/space.h"
#include "engine/runtime/systems/collisionWorld3D.h"

#include <functional>
#include <memory>
#include <vector>

namespace sfs::nav
{

/** What a vehicle's boxes stand on: their span (m, round the reference
 * point: back..front along it, its sides `half` out). */
struct Span
{
  float back = 0.0f, front = 0.0f, half = 0.0f;
};
/** The span of `boxes` (all of them: a rig held rigid tilts as one). */
Span spanOf(std::span<const Box3> boxes);

/** Where a vehicle's boxes really are, standing at `pose` on the ground. */
struct Stance
{
  glm::vec3 position;
  glm::quat rotation;
};
/**
 * THE stance of a vehicle on uneven ground: at the ground under its
 * reference point, yawed to its heading and tilted as the ground under it
 * lies: pitched by the ground at its back and front ends, rolled by the
 * ground at its two sides (its span). The planner's checks (CollisionSpace)
 * and a game's live look-ahead stand the boxes by this one function, so a
 * tall tool's top on a slope is where the physics has it. No ground: level.
 */
Stance stanceOf(Pose2 pose,
                const Span& span,
                const std::function<float(glm::vec2)>& ground);

/**
 * Each collider's footprint on the ground as it stands, from one frozen
 * world: a box's outline yawed as it's turned (tipped: the upright box
 * round it), a sphere's or a capsule's circle (`circle`, `half.x` its
 * radius), with its height band (`bottom`..`top`, metres over the ground
 * under its middle: `ground`, default 0) and its body's id. Only bodies in
 * `layers` and not in `ignore`. THE one place a collider's 2D outline is
 * worked out: CollisionSpace::raster() and anything that traces what's solid
 * in 2D (a field planner's obstacle outlines, a game's cells) take it from
 * here, never from a copy of their own.
 */
std::vector<BoxSpace::Solid>
footprints(const CollisionWorld3D& world,
           uint32_t layers = ~0u,
           std::span<const uint64_t> ignore = {},
           const std::function<float(glm::vec2)>& ground = {});

/**
 * The navigation Space of engine colliders: one or more frozen
 * CollisionWorld3Ds (the scene's static snapshot from
 * PhysicsSystem3D::snapshot(), parked machines in another), queried with the
 * vehicle's boxes as exact Jolt shapes, so a plan touches exactly what the
 * physics would. Heights count: a box only meets what its bottom..top
 * reaches (a raised header clears a low rail). Safe from any thread.
 */
class CollisionSpace : public Space
{
public:
  struct Options
  {
    uint32_t layers = ~0u;                  // the colliders' layers that count
    std::vector<uint64_t> ignore;           // ids left out (its own parts)
    std::function<float(glm::vec2)> ground; // ground height (default 0)
    float rasterLow = 0.05f, rasterHigh = 4.5f; // what the 2D heuristic counts
  };
  CollisionSpace(std::vector<std::shared_ptr<const CollisionWorld3D>> worlds,
                 Options options);

  std::shared_ptr<const void> prepare(std::span<const Box3> boxes,
                                      float grow,
                                      float extend,
                                      float widen) const override;
  bool fits(const void* prepared, Pose2 pose) const override;
  std::optional<Hit> hit(const void* prepared, Pose2 pose) const override;
  void raster(glm::vec2 origin,
              float cell,
              int width,
              int height,
              float inflate,
              std::vector<uint8_t>& out) const override;
  Rect bounds() const override { return m_bounds; }
  /** Its colliders' footprints (sfs::nav::footprints() of every world, by
   * its layers, ignored ids and ground). */
  std::vector<BoxSpace::Solid> footprints() const;

private:
  std::vector<std::shared_ptr<const CollisionWorld3D>> m_worlds;
  Options m_options;
  Rect m_bounds;
};

} // namespace sfs::nav
