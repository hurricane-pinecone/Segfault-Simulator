#include "engine/runtime/facilities/physics3D.h"

#include "absentBackend.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/rendering/camera.h"
#include "engine/runtime/systems/collisionWorld3D.h"
#include "engine/runtime/systems/physicsSystem3D.h"
#include "engine/runtime/systems/raycastHit3D.h"

#include <glm/glm/ext/vector_float3.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace sfs::facility
{

namespace
{
std::atomic<bool> s_warned{false};
} // namespace

void Physics3D::addForce(const Entity& entity, const glm::vec3& force) const
{
  if (PhysicsSystem3D* physics = backend())
    physics->addForce(entity, force);
}

void Physics3D::addImpulse(const Entity& entity, const glm::vec3& impulse) const
{
  if (PhysicsSystem3D* physics = backend())
    physics->addImpulse(entity, impulse);
}

glm::vec3 Physics3D::angularVelocity(const Entity& entity) const
{
  const PhysicsSystem3D* physics = backend();
  return physics ? physics->angularVelocity(entity) : glm::vec3(0.0f);
}

PhysicsSystem3D* Physics3D::backend() const
{
  if (!m_physics)
    warnAbsent(s_warned, "physics", "PhysicsSystem3D");

  return m_physics;
}

glm::vec3 Physics3D::gravity() const
{
  const PhysicsSystem3D* physics = backend();
  return physics ? physics->gravity() : glm::vec3(0.0f);
}

bool Physics3D::hasBody(const Entity& entity) const
{
  const PhysicsSystem3D* physics = backend();
  return physics && physics->hasBody(entity);
}

Physics3D Physics3D::in(Registry* registry)
{
  return Physics3D(registry ? registry->tryGetSystem<PhysicsSystem3D>()
                            : nullptr);
}

std::optional<RaycastHit3D> Physics3D::raycast(const Ray& ray,
                                               float maxDistance) const
{
  const PhysicsSystem3D* physics = backend();
  return physics ? physics->raycast(ray, maxDistance) : std::nullopt;
}

uint64_t Physics3D::revision() const
{
  const PhysicsSystem3D* physics = backend();
  return physics ? physics->revision() : 0;
}

void Physics3D::setAngularVelocity(const Entity& entity,
                                   const glm::vec3& radiansPerSecond) const
{
  if (PhysicsSystem3D* physics = backend())
    physics->setAngularVelocity(entity, radiansPerSecond);
}

void Physics3D::setGravity(const glm::vec3& gravity) const
{
  if (PhysicsSystem3D* physics = backend())
    physics->setGravity(gravity);
}

void Physics3D::setVelocity(const Entity& entity,
                            const glm::vec3& velocity) const
{
  if (PhysicsSystem3D* physics = backend())
    physics->setVelocity(entity, velocity);
}

std::shared_ptr<const CollisionWorld3D>
Physics3D::snapshot(const std::function<bool(const Entity&)>& include) const
{
  const PhysicsSystem3D* physics = backend();
  return physics ? physics->snapshot(include) : nullptr;
}

glm::vec3 Physics3D::velocity(const Entity& entity) const
{
  const PhysicsSystem3D* physics = backend();
  return physics ? physics->velocity(entity) : glm::vec3(0.0f);
}

} // namespace sfs::facility
