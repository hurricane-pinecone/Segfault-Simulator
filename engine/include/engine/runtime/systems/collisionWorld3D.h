#pragma once

#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/gtc/quaternion.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sfs
{

/**
 * One shape of a body or a probe: a box (full `size`), a sphere (`size.x`
 * its radius) or an upright capsule (`size.x` radius, `size.y` total height,
 * along Z), placed at `offset` and turned by `rotation` in its body's frame.
 */
struct CollisionShape3D
{
  enum class Kind
  {
    Box,
    Sphere,
    Capsule,
  };
  Kind kind = Kind::Box;
  glm::vec3 size{1.0f};
  glm::vec3 offset{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/**
 * A frozen copy of solid bodies for asking "would this fit here?" away from
 * the simulation: a planner trying a vehicle's boxes at thousands of poses,
 * on worker threads, while the game goes on. Built once from a list of bodies
 * (PhysicsSystem3D::snapshot() makes one of the scene's), it never changes,
 * and any number of threads may query it at once.
 *
 * Queries take a Probe (shapes built once, tried at many poses), a pose, a
 * mask of the body layers that count (bit n: layer n, the colliders' `layer`)
 * and ids to leave out (a vehicle's own parts).
 */
class CollisionWorld3D
{
public:
  struct Body
  {
    uint64_t id = 0;    // the caller's (PhysicsSystem3D: the entity's id)
    uint32_t layer = 0; // 0..31
    std::vector<CollisionShape3D> shapes;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  };

  /** Shapes to try at poses: built once (it's the costly part), reused. */
  class Probe
  {
  public:
    Probe() = default;
    explicit Probe(std::span<const CollisionShape3D> shapes);
    bool empty() const { return !m_shape; }

  private:
    friend class CollisionWorld3D;
    std::shared_ptr<const void> m_shape; // (a Jolt shape)
  };

  struct Hit
  {
    uint64_t id = 0;
    uint32_t layer = 0;
    glm::vec3 point{0.0f}; // where they touch (on the body hit)
    float depth = 0.0f;    // how far they overlap
  };

  explicit CollisionWorld3D(std::span<const Body> bodies);
  ~CollisionWorld3D();
  CollisionWorld3D(const CollisionWorld3D&) = delete;
  CollisionWorld3D& operator=(const CollisionWorld3D&) = delete;

  /** Whether the probe at the pose overlaps any body in `layers`. */
  bool overlaps(const Probe& probe,
                const glm::vec3& position,
                const glm::quat& rotation,
                uint32_t layers = ~0u,
                std::span<const uint64_t> ignore = {}) const;
  /** The deepest overlap, if any. */
  std::optional<Hit> overlap(const Probe& probe,
                             const glm::vec3& position,
                             const glm::quat& rotation,
                             uint32_t layers = ~0u,
                             std::span<const uint64_t> ignore = {}) const;
  /**
   * Swept in a straight line from `from` to `to` (turned `fromRotation` all
   * the way): how far along (0..1) it first touches a body, if it does.
   */
  std::optional<float> cast(const Probe& probe,
                            const glm::vec3& from,
                            const glm::quat& fromRotation,
                            const glm::vec3& to,
                            uint32_t layers = ~0u,
                            std::span<const uint64_t> ignore = {}) const;

  std::size_t size() const;
  /** The bodies it was built from (their shapes, poses, ids and layers). */
  const std::vector<Body>& bodies() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

/** Yaw about Z, the usual turn of something standing on the ground. */
inline glm::quat yawRotation(float yaw)
{
  return glm::quat(std::cos(0.5f * yaw), 0.0f, 0.0f, std::sin(0.5f * yaw));
}

} // namespace sfs
