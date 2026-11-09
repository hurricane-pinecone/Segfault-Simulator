#pragma once

#include "engine/runtime/navigation/geometry.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sfs::nav
{

/**
 * What's solid, as the planner asks it: "would these boxes, placed so, touch
 * anything?" thousands of times a plan, from worker threads (an
 * implementation must be safe to query from several threads at once and
 * must not change while a plan uses it: hand plans an immutable snapshot).
 * The runtime's CollisionSpace answers from engine colliders
 * (CollisionWorld3D); BoxSpace below is a plain one for tests and tools.
 */
class Space
{
public:
  virtual ~Space() = default;
  /**
   * Shapes prepared once for many queries: `boxes` grown `grow` all round,
   * stretched `extend` more at both ends and `widen` more at both sides (a
   * pose standing for the stretch of motion either side of it).
   */
  virtual std::shared_ptr<const void> prepare(std::span<const Box3> boxes,
                                              float grow,
                                              float extend,
                                              float widen = 0.0f) const = 0;
  /** Whether prepared shapes at `pose` touch nothing. */
  virtual bool fits(const void* prepared, Pose2 pose) const = 0;
  struct Hit
  {
    uint64_t id = 0;
    glm::vec2 point{0.0f};
  };
  /** What they touch there, if anything. */
  virtual std::optional<Hit> hit(const void* prepared, Pose2 pose) const = 0;
  /**
   * The solid cells of a raster (row-major, `width` x `height` from
   * `origin`): 1 where something solid comes within `inflate` of the cell's
   * middle. For the search's 2D heuristic only, so it may be conservative
   * the other way (marking too little), never the only check.
   */
  virtual void raster(glm::vec2 origin,
                      float cell,
                      int width,
                      int height,
                      float inflate,
                      std::vector<uint8_t>& out) const = 0;
  /** Where there's anything at all (a search never needs to go further). */
  virtual Rect bounds() const = 0;
};

/**
 * A Space of upright boxes and circles listed in code: tests, tools and
 * small games without physics. Brute force (fine for a few hundred).
 */
class BoxSpace : public Space
{
public:
  struct Solid
  {
    glm::vec2 centre{0.0f};
    glm::vec2 half{0.5f}; // a box's half sizes (radius: half.x, circle)
    float yaw = 0.0f;
    float bottom = 0.0f, top = 2.0f;
    bool circle = false;
    uint64_t id = 0;
  };
  explicit BoxSpace(std::vector<Solid> solids);

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
  std::vector<Solid> m_solids;
  Rect m_bounds;
};

/**
 * A Space and a few more solids on top (parked agents, another agent's
 * claim while one clears out of its way): solid where either is.
 */
class SpaceWithSolids : public Space
{
public:
  SpaceWithSolids(std::shared_ptr<const Space> base,
                  std::vector<BoxSpace::Solid> solids);

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
  Rect bounds() const override;

private:
  std::shared_ptr<const Space> m_base;
  bool m_none = true;
  BoxSpace m_solids;
};

} // namespace sfs::nav
