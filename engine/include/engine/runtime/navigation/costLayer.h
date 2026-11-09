#pragma once

#include <memory>
#include <vector>

#include "engine/runtime/navigation/geometry.h"

#include <vector>

namespace sfs::nav
{

/**
 * What ground costs to drive over, as a factor on time (1: ordinary ground;
 * a worn track 0.85; rough ground more; infinity: never, for that mode: a
 * combine's body over standing crop). Thread-safe and fixed for a plan, as
 * Space is. `mode` is the VehicleModel mode the pose is driven in.
 */
class CostLayer
{
public:
  virtual ~CostLayer() = default;
  virtual float factor(Pose2 pose, int mode) const = 0;
  /** The lowest factor anywhere (the heuristic stays admissible). */
  virtual float floor() const { return 1.0f; }
  /**
   * Lower bounds of the factor over a raster's cells (the 2D heuristic);
   * the default is floor() everywhere.
   */
  virtual void raster(glm::vec2 origin,
                      float cell,
                      int width,
                      int height,
                      int mode,
                      std::vector<float>& out) const;
  /** Where nothing may stop and wait (gateways, a working corridor). */
  virtual bool noWait(glm::vec2) const { return false; }
};

/** Ordinary ground everywhere. */
class FlatCost : public CostLayer
{
public:
  float factor(Pose2, int) const override { return 1.0f; }
};

/**
 * Several layers as one (NavigationSystem's: each a game's, by name): the
 * factors multiply (a worn track over rough ground), nothing may wait where
 * any says so.
 */
class CombinedCost : public CostLayer
{
public:
  explicit CombinedCost(std::vector<std::shared_ptr<const CostLayer>> layers)
      : m_layers(std::move(layers))
  {
  }
  float factor(Pose2 pose, int mode) const override;
  float floor() const override;
  void raster(glm::vec2 origin,
              float cell,
              int width,
              int height,
              int mode,
              std::vector<float>& out) const override;
  bool noWait(glm::vec2 p) const override;

private:
  std::vector<std::shared_ptr<const CostLayer>> m_layers;
};

} // namespace sfs::nav
