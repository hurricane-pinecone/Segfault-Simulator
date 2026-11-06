#pragma once

#include "engine/core/ecs/system.h"
#include "engine/core/rendering/camera.h"
#include "engine/runtime/systems/collision3D.h"
#include "engine/runtime/systems/collisionWorld3D.h"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace sfs
{

class Scene;

/** Where a raycast hit a physics body. */
struct RaycastHit3D
{
  Entity entity;
  glm::vec3 point{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  float distance = 0.0f;
};

/**
 * 3D physics for a Z-up world (built on Jolt Physics). Every entity with a
 * TransformComponent and a collider (box, sphere or capsule) is a body:
 * static on its own, dynamic or kinematic with a RigidBody3DComponent.
 *
 * Each update it picks up bodies that were added, removed or changed, steps
 * the simulation at a fixed 60 steps a second, and writes dynamic bodies'
 * positions and rotations back into their transforms (parents included, so a
 * dynamic body may be a child). Moving a body's transform from code teleports
 * it; for a kinematic body it's a move that pushes what's in the way.
 *
 * A scene with no dynamic bodies has nothing to simulate, only contacts to
 * find: it steps only when a body moved, appeared or went, in steps short
 * enough that no moving body passes anything unnoticed, so its cost doesn't
 * grow with the game's speed. Bodies are checked against their components
 * every update (a compare each); the scene is searched for new ones only when
 * an entity gains or loses a collider or rigid body.
 *
 * Contacts that begin or end (touching, or passing through a trigger) are
 * told to the game objects on both entities (GameObject::onCollisionEnter and
 * the rest) when it knows its scene, and kept as contactsBegun() and
 * contactsEnded().
 *
 * A scene lists it by name ("PhysicsSystem3D") to have physics. The editor
 * keeps it disabled while editing, so nothing falls until Play.
 */
class PhysicsSystem3D : public System
{
public:
  /** `scene`: whose game objects hear about contacts (none: nobody's). */
  explicit PhysicsSystem3D(Scene* scene = nullptr);
  ~PhysicsSystem3D() override;

  PhysicsSystem3D(const PhysicsSystem3D&) = delete;
  PhysicsSystem3D& operator=(const PhysicsSystem3D&) = delete;

  /** Gravity in world units per second squared; default (0, 0, -9.81). */
  void setGravity(const glm::vec3& gravity);
  glm::vec3 gravity() const;

  /**
   * The nearest body the ray hits within `maxDistance` (triggers are ignored),
   * as of the last update. Pair with RenderSystem::screenRay to find what
   * the mouse is over.
   */
  std::optional<RaycastHit3D> raycast(const Ray& ray,
                                      float maxDistance = 1000.0f) const;

  // A dynamic body's motion; no effect on an entity that isn't one.
  glm::vec3 velocity(const Entity& entity) const;
  void setVelocity(const Entity& entity, const glm::vec3& velocity);
  glm::vec3 angularVelocity(const Entity& entity) const;
  void setAngularVelocity(const Entity& entity,
                          const glm::vec3& radiansPerSecond);
  /** A sudden push (mass * velocity change), e.g. a jump or a hit. */
  void addImpulse(const Entity& entity, const glm::vec3& impulse);
  /** A steady push, for the next step (call it every frame to keep pushing). */
  void addForce(const Entity& entity, const glm::vec3& force);

  /** Whether the entity currently has a physics body. */
  bool hasBody(const Entity& entity) const;

  /**
   * A frozen copy of the solid bodies that aren't dynamic (static ones, and
   * kinematic ones where they are now), as of the last update, for queries on
   * any thread (CollisionWorld3D): a planner trying shapes at many poses.
   * `include`: which entities' bodies to copy (none given: all). A body's ids
   * are its entity's (Entity::getId()); its layer the lowest of its colliders'.
   */
  std::shared_ptr<const CollisionWorld3D>
  snapshot(const std::function<bool(const Entity&)>& include = {}) const;
  /**
   * Goes up whenever a body is added, removed or rebuilt, or a static one is
   * moved: a snapshot of static bodies is current while this hasn't changed.
   */
  uint64_t revision() const;

  /** What the last update did, for tests and measuring. */
  struct UpdateStats
  {
    int steps = 0;                // simulation steps taken
    bool lookedForBodies = false; // searched the scene for new bodies
  };
  const UpdateStats& lastUpdateStats() const;

  /** Contacts that began and ended during the last update. */
  const std::vector<Contact3D>& contactsBegun() const;
  const std::vector<Contact3D>& contactsEnded() const;

protected:
  void update(double deltaTime) override;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
  Scene* m_scene = nullptr;
};

} // namespace sfs
