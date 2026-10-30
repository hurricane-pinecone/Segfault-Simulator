#include "engine/runtime/systems/physicsSystem2D.h"

#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/rigidBody2DComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/sceneManager/scene.h"
#include "glm/glm/common.hpp"

#include <cmath>

namespace sfs
{

namespace
{
// A collider where it is this frame, in the world.
struct Box
{
  Entity entity;
  glm::vec2 min{0.0f};
  glm::vec2 max{0.0f};
  long level = 0; // the height it's at, rounded
  bool trigger = false;
  bool body = false; // a RigidBody2DComponent's (not static)
};

Box boxOf(const Entity& entity, const TransformComponent& world)
{
  const auto& collider = entity.getComponent<BoxCollider2D>();
  const glm::vec2 centre = world.position2D() + collider.offset;
  const glm::vec2 half = glm::abs(collider.half);
  return Box{entity,
             centre - half,
             centre + half,
             std::lround(world.position.z),
             collider.trigger,
             entity.hasComponent<RigidBody2DComponent>()};
}

// Whether two boxes overlap by more than `skin` on both axes (a negative
// skin: whether they touch, or come within -skin).
bool overlaps(const Box& a, const Box& b, float skin)
{
  return a.min.x < b.max.x - skin && a.max.x > b.min.x + skin &&
         a.min.y < b.max.y - skin && a.max.y > b.min.y + skin;
}

// Which way b pushes a: out along the axis they overlap least on.
glm::vec2 separation(const Box& a, const Box& b)
{
  const float x = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
  const float y = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
  const glm::vec2 apart = (a.min + a.max) - (b.min + b.max);
  if (x < y)
    return {apart.x < 0.0f ? -1.0f : 1.0f, 0.0f};
  return {0.0f, apart.y < 0.0f ? -1.0f : 1.0f};
}

// How far a box moving `step` along `axis` gets before a solid stops it; the
// solids it starts inside of (or beside, not across its way) don't.
float sweep(const Box& moving,
            int axis,
            float step,
            const std::vector<Box>& solids,
            float skin)
{
  const int other = 1 - axis;
  for (const Box& solid : solids)
  {
    if (solid.level != moving.level ||
        moving.min[other] >= solid.max[other] - skin ||
        moving.max[other] <= solid.min[other] + skin)
      continue;
    if (step > 0.0f && moving.max[axis] <= solid.min[axis] + skin)
      step = std::min(step, std::max(solid.min[axis] - moving.max[axis], 0.0f));
    else if (step < 0.0f && moving.min[axis] >= solid.max[axis] - skin)
      step = std::max(step, std::min(solid.max[axis] - moving.min[axis], 0.0f));
  }
  return step;
}

uint64_t pairKey(const Entity& a, const Entity& b)
{
  const uint64_t low = std::min(a.getId(), b.getId());
  const uint64_t high = std::max(a.getId(), b.getId());
  return (high << 32) | low;
}
} // namespace

PhysicsSystem2D::PhysicsSystem2D(Scene* scene) : m_scene(scene)
{
  registerComponent<TransformComponent>();
  registerComponent<RigidBody2DComponent>();
}

void PhysicsSystem2D::update(double deltaTime)
{
  m_begun.clear();
  m_ended.clear();
  if (!registry)
    return;
  const float dt = static_cast<float>(deltaTime);

  // The statics that stop bodies: their solid boxes, as they are now.
  std::vector<Box> solids;
  for (const Entity& entity :
       registry->view<BoxCollider2D, TransformComponent>())
    if (!entity.hasComponent<RigidBody2DComponent>() &&
        !entity.getComponent<BoxCollider2D>().trigger)
      solids.push_back(boxOf(entity, hierarchy::worldTransform(entity)));

  for (const Entity& entity : getEntities())
  {
    auto& body = entity.getComponent<RigidBody2DComponent>();
    TransformComponent world = hierarchy::worldTransform(entity);
    body.previousPosition = world.position2D();
    body.velocity += body.gravity * dt;
    glm::vec2 step = body.velocity * dt;
    if (step == glm::vec2(0.0f))
      continue;

    // Across, then up or down: each axis stops at the first solid in the way.
    if (entity.hasComponent<BoxCollider2D>() &&
        !entity.getComponent<BoxCollider2D>().trigger)
    {
      Box box = boxOf(entity, world);
      for (int axis = 0; axis < 2; ++axis)
      {
        const float wanted = step[axis];
        step[axis] = sweep(box, axis, wanted, solids, kSkin);
        box.min[axis] += step[axis];
        box.max[axis] += step[axis];
        if (step[axis] != wanted)
          body.velocity[axis] = 0.0f;
      }
    }
    world.setPosition2D(world.position2D() + step);
    hierarchy::setWorldTransform(entity, world);
  }

  // What touches what now: every collider against the others, bodies first
  // (two statics never meet).
  std::vector<Box> boxes;
  for (const Entity& entity :
       registry->view<BoxCollider2D, TransformComponent>())
    boxes.push_back(boxOf(entity, hierarchy::worldTransform(entity)));
  std::unordered_map<uint64_t, Contact2D> touching;
  for (std::size_t i = 0; i < boxes.size(); ++i)
    for (std::size_t j = 0; j < boxes.size(); ++j)
    {
      const Box& a = boxes[i];
      const Box& b = boxes[j];
      // Each pair once: a body with any static, or two bodies in order.
      if (!a.body || (b.body && j <= i) || a.level != b.level)
        continue;
      const bool trigger = a.trigger || b.trigger;
      // A trigger: overlapping it. A solid: touching (within the skin).
      if (!overlaps(a, b, trigger ? kSkin : -kSkin))
        continue;
      touching.emplace(pairKey(a.entity, b.entity),
                       Contact2D{a.entity,
                                 b.entity,
                                 trigger ? glm::vec2(0.0f) : separation(a, b),
                                 trigger});
    }

  for (const auto& [key, contact] : touching)
    if (!m_touching.contains(key))
      m_begun.push_back(contact);
  for (const auto& [key, contact] : m_touching)
    if (!touching.contains(key))
      m_ended.push_back(contact);
  m_touching = std::move(touching);

  if (!m_scene)
    return;
  // Copied: a handler may destroy entities or switch scenes.
  const std::vector<Contact2D> begun = m_begun;
  const std::vector<Contact2D> ended = m_ended;
  for (const Contact2D& contact : begun)
    m_scene->notifyContact(contact, true);
  for (const Contact2D& contact : ended)
    m_scene->notifyContact(contact, false);
}

} // namespace sfs
