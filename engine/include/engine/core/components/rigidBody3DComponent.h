#pragma once

namespace sfs
{

/**
 * Makes the entity a moving physics body. It needs at least one collider
 * (BoxCollider3DComponent, SphereCollider3DComponent or
 * CapsuleCollider3DComponent) for its shape. An entity with colliders but no
 * rigid body is static: other bodies hit it, but it never moves.
 *
 * While the scene plays, PhysicsSystem3D moves a dynamic body and writes its
 * position and rotation into the entity's transform. A kinematic body is the
 * other way round: game code moves its transform, and it pushes dynamic bodies
 * out of the way without being pushed back (a door, a platform, a character
 * driven by script).
 *
 * @param float mass - in kilograms (dynamic bodies)
 * @param bool kinematic - moved by its transform instead of by physics
 * @param float friction - 0 = ice, 1 = rubber
 * @param float bounciness - 0 = no bounce, 1 = bounces back fully
 * @param float linearDamping - how quickly it slows down by itself
 * @param float angularDamping - how quickly it stops spinning by itself
 * @param float gravityScale - 1 = normal gravity, 0 = floats
 * @param bool lockRotation - never tips over or spins (e.g. a character)
 * @param bool bendsGrass - flattens grass it moves through (GrassComponent),
 *                          sized by its colliders
 */
struct RigidBody3DComponent
{
  [[sfs::range(0.01, 1000)]] float mass = 1.0f;
  bool kinematic = false;
  [[sfs::range(0, 1)]] float friction = 0.5f;
  [[sfs::range(0, 1)]] float bounciness = 0.0f;
  [[sfs::range(0, 1)]] float linearDamping = 0.05f;
  [[sfs::range(0, 1)]] float angularDamping = 0.05f;
  [[sfs::range(-2, 2)]] float gravityScale = 1.0f;
  bool lockRotation = false;
  bool bendsGrass = true;
};

} // namespace sfs
