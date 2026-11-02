#pragma once

#include "engine/core/ecs/system.h"
#include "engine/runtime/systems/collision2D.h"
#include "glm/glm/ext/vector_float2.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sfs
{

class Scene;

/**
 * Physics for a 2D game (flat or isometric), on its plane. Each update every
 * RigidBody2DComponent body speeds up by its gravity and moves by its
 * velocity; one with a BoxCollider2D is stopped flush against static boxes
 * (BoxCollider2Ds on entities without a body) in its way. The move is swept,
 * one axis then the other, so a fast body can't pass through a thin wall,
 * and slides along what it hits; the velocity into it is zeroed.
 *
 * Bodies don't push each other (they pass through), but are told they touch.
 * Triggers block nothing. Contacts (touching, or overlapping a trigger) that
 * begin or end are reported: to the game objects on both entities
 * (GameObject::onCollisionEnter2D and the rest), when the system knows its
 * scene, and as contactsBegun()/contactsEnded().
 *
 * Positions are the entities' world positions on the plane (a body may be a
 * child: it's moved in the world). Only things at the same height (the
 * transform's z, rounded) meet. A scene lists it by name ("PhysicsSystem2D").
 */
class PhysicsSystem2D : public System
{
public:
  /** `scene`: whose game objects hear about contacts (none: nobody's). */
  explicit PhysicsSystem2D(Scene* scene = nullptr);

  /** Contacts that began and ended during the last update. */
  const std::vector<Contact2D>& contactsBegun() const { return m_begun; }
  const std::vector<Contact2D>& contactsEnded() const { return m_ended; }

protected:
  void update(double deltaTime) override;

private:
  // How near two boxes count as touching, and how far one may sink into
  // another before it's taken as already inside (not hit).
  static constexpr float kSkin = 1.0e-3f;

  Scene* m_scene = nullptr;
  std::vector<Contact2D> m_begun;
  std::vector<Contact2D> m_ended;
  // The contacts going on, by pair of entity ids (lower first).
  std::unordered_map<uint64_t, Contact2D> m_touching;
};

} // namespace sfs
