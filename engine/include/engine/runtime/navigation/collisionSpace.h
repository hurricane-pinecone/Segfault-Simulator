#pragma once

#include "engine/runtime/navigation/space.h"
#include "engine/runtime/systems/collisionWorld3D.h"

#include <functional>
#include <memory>
#include <vector>

namespace sfs::nav
{

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

private:
  glm::vec3 at(Pose2 pose) const;
  std::vector<std::shared_ptr<const CollisionWorld3D>> m_worlds;
  Options m_options;
  Rect m_bounds;
};

} // namespace sfs::nav
