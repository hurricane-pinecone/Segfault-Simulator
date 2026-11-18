#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/core/rendering/camera.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/systems/collisionWorld3D.h"
#include "engine/runtime/systems/raycastHit3D.h"

#include <glm/glm/ext/vector_float3.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace sfs
{

class PhysicsSystem3D;

namespace facility
{

/**
 * The scene's 3D physics, as game code uses it (`physics()` on a System,
 * Service, GameObject or Scene): raycasts and frozen snapshots of the solids,
 * a body's motion, and pushes. A value handle over the scene's
 * PhysicsSystem3D, valid for the step that got it: get it where it's used,
 * keep it in no member, and hand workers its snapshot() instead. In a scene
 * without physics its commands do nothing and its queries give empty
 * answers, with one warning a run (available() tells).
 */
class Physics3D
{
public:
  /**
   * A steady push on a dynamic body, for the next step (push every frame to
   * keep pushing).
   * @param entity the body's entity
   * @param force the push, in world units
   */
  void addForce(const Entity& entity, const glm::vec3& force) const;

  /**
   * A sudden push (mass * velocity change) on a dynamic body: a jump, a hit.
   * @param entity the body's entity
   * @param impulse the push, in world units
   */
  void addImpulse(const Entity& entity, const glm::vec3& impulse) const;

  /**
   * A dynamic body's spin.
   * @param entity the body's entity
   * @return radians a second about each axis; zero for no dynamic body
   */
  glm::vec3 angularVelocity(const Entity& entity) const;

  /** @return whether the scene has physics behind this facade */
  inline bool available() const { return m_physics != nullptr; }

  /** @return gravity, world units a second squared; zero without physics */
  glm::vec3 gravity() const;

  /**
   * Whether an entity has a physics body now.
   * @param entity the entity
   * @return true if it has one
   */
  bool hasBody(const Entity& entity) const;

  /**
   * The physics of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade, absent when it has no PhysicsSystem3D
   */
  static Physics3D in(Registry* registry);

  /**
   * The nearest body a ray hits, triggers ignored, as of the last physics
   * update.
   * @param ray the ray (cameras().screenRay() for the mouse)
   * @param maxDistance how far along the ray to look
   * @return the hit; none if it hits nothing
   */
  std::optional<RaycastHit3D> raycast(const Ray& ray,
                                      float maxDistance = 1000.0f) const;

  /**
   * Goes up whenever a body is added, removed or rebuilt, or a static one is
   * moved: a snapshot() is current while it hasn't changed.
   * @return the revision; 0 without physics
   */
  uint64_t revision() const;

  /**
   * Sets a dynamic body's spin.
   * @param entity the body's entity
   * @param radiansPerSecond its spin about each axis
   */
  void setAngularVelocity(const Entity& entity,
                          const glm::vec3& radiansPerSecond) const;

  /**
   * Sets the scene's gravity (default (0, 0, -9.81)).
   * @param gravity world units a second squared
   */
  void setGravity(const glm::vec3& gravity) const;

  /**
   * Sets a dynamic body's motion.
   * @param entity the body's entity
   * @param velocity world units a second
   */
  void setVelocity(const Entity& entity, const glm::vec3& velocity) const;

  /**
   * A frozen copy of the solid bodies that aren't dynamic, as of the last
   * update, for queries on any thread (a planner trying shapes at many
   * poses). It outlives the physics: hand it to workers.
   * @param include which entities' bodies to copy (none given: all)
   * @return the copy; null without physics
   */
  std::shared_ptr<const CollisionWorld3D>
  snapshot(const std::function<bool(const Entity&)>& include = {}) const;

  /**
   * A dynamic body's motion.
   * @param entity the body's entity
   * @return world units a second; zero for no dynamic body
   */
  glm::vec3 velocity(const Entity& entity) const;

private:
  inline explicit Physics3D(PhysicsSystem3D* physics) : m_physics(physics) {}

  // The physics; null (and warned about once) when the scene has none.
  PhysicsSystem3D* backend() const;

  PhysicsSystem3D* m_physics = nullptr;
};

} // namespace facility

inline facility::Physics3D System::physics() const
{
  return facility::Physics3D::in(registry);
}

inline facility::Physics3D Service::physics() const
{
  return facility::Physics3D::in(m_registry);
}

inline facility::Physics3D GameObject::physics() const
{
  return m_scene ? m_scene->physics() : facility::Physics3D::in(nullptr);
}

inline facility::Physics3D Scene::physics()
{
  return facility::Physics3D::in(&m_registry);
}

} // namespace sfs
