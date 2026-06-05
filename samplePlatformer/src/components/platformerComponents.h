#pragma once

#include "glm/glm/ext/vector_float2.hpp"

// Game-local components for the platformer. These are plain structs used as ECS
// components against the engine's generic registry -- no engine changes needed.
namespace platformer
{

// Axis-aligned box collider, half-extents in world pixels, centred on the
// entity's TransformComponent.position.
struct BoxCollider
{
  glm::vec2 half{8.0f, 8.0f};

  BoxCollider() = default;
  explicit BoxCollider(glm::vec2 half) : half(half) {}
};

// Tags a static, immovable, solid platform that dynamic bodies collide with.
struct Solid
{
};

// Marks a dynamic body affected by gravity and platform collision. onGround is
// written by PlatformerPhysicsSystem each frame (true when standing on a solid).
struct PlatformerBody
{
  bool onGround = false;
};

// An enemy that chases the player (walking off platform edges to drop down) and
// takes bullet damage.
struct Enemy
{
  float health = 30.0f;

  Enemy() = default;
  explicit Enemy(float health) : health(health) {}
};

// Marks the player entity so systems (e.g. enemy chase AI) can find its position
// through the registry.
struct PlayerTag
{
};

// A straight-flying projectile. Moves by its own velocity (no gravity) and
// expires after `life` seconds or on hitting an enemy.
struct Bullet
{
  glm::vec2 velocity{0.0f, 0.0f};
  float life = 1.0f;

  Bullet() = default;
  Bullet(glm::vec2 velocity, float life) : velocity(velocity), life(life) {}
};

// Destroys its entity after `remaining` seconds. Powers transient effects like
// muzzle-flash and death-flash lights.
struct Lifetime
{
  float remaining = 1.0f;

  Lifetime() = default;
  explicit Lifetime(float remaining) : remaining(remaining) {}
};

} // namespace platformer
