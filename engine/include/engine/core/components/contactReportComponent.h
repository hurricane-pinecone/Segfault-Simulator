#pragma once

#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/vector_float3.hpp"

#include <vector>

namespace sfs
{

/**
 * A contact between two physics bodies that began, went deeper or ended during
 * a step.
 */
struct Contact3D
{
  /** One of the two bodies' entities (in a ContactReportComponent: its own). */
  Entity a;
  /** The other body's entity. */
  Entity b;
  /** One of them is a trigger (nothing was blocked). */
  bool trigger = false;
  /** Where they first touched (began contacts). */
  glm::vec3 point{0.0f};
  /**
   * How far they overlap, in world units: as they began touching (began), or
   * the deepest since then (deepened, ended). 0 for triggers.
   */
  float depth = 0.0f;
};

/**
 * Collects the physics contacts of the colliders on its entity and on its
 * descendants, as Unity sends a rigidbody the collisions of its child
 * colliders. A descendant with its own ContactReportComponent reports its own
 * subtree instead. Put it on a machine's root to hear every part of it touch
 * something, and read it from a system each update.
 *
 * PhysicsSystem3D clears it at the start of each of its updates and fills it
 * with that update's contacts, so it holds the contacts of the last physics
 * update. In each contact `a` is one of its own colliders' entities and `b`
 * the other body's. A contact between two of its own colliders is reported
 * once. Two entities with reports each hear a contact between them.
 */
struct ContactReportComponent
{
  /** Contacts that began during the last physics update. */
  [[sfs::hide]] std::vector<Contact3D> begun;
  /** Contacts that ended during the last physics update. */
  [[sfs::hide]] std::vector<Contact3D> ended;
  /**
   * Contacts that went deeper during the last physics update, with their
   * deepest depth (see PhysicsSystem3D::contactsDeepened). Not triggers.
   */
  [[sfs::hide]] std::vector<Contact3D> deepened;
};

} // namespace sfs
