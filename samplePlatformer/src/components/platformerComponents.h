#pragma once

#include "spells.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <vector>

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

// A projectile carrying its accumulated spell modifiers. Built by the player
// from its Loadout; the BulletSystem reads these to steer, bounce, pierce,
// detonate, chain, and drop. Plain aggregate so the player can fill the fields
// then copy it in via addComponent.
struct Bullet
{
  glm::vec2 velocity{0.0f, 0.0f};
  float life = 1.0f;
  float damage = 12.0f;

  int bounces = 0;        // ricochets off platforms remaining (Bounce)
  bool pierce = false;    // pass through enemies (Pierce)
  bool homing = false;    // steer toward nearest enemy (Homing)
  bool explosive = false; // detonate on impact (Explosive)
  bool chain = false;     // arc lightning to nearby enemies on hit (Chain)
  float gravity = 0.0f;   // downward accel (Gravity)

  glm::vec3 color{1.0f, 0.2f, 0.12f}; // bolt light / effect tint
};

// Destroys its entity after `remaining` seconds. Powers transient effects like
// muzzle / death / explosion flash lights.
struct Lifetime
{
  float remaining = 1.0f;

  Lifetime() = default;
  explicit Lifetime(float remaining) : remaining(remaining) {}
};

// Marks the player entity so systems (enemy chase, pickup collection) can find
// it through the registry.
struct PlayerTag
{
};


// The player's collected spells. Every shot applies all of them, stacked.
struct Loadout
{
  std::vector<Spell> spells;
};

// A floating pickup orb that grants one spell when the player touches it.
struct SpellPickup
{
  Spell spell = Spell::Triple;
  float bob = 0.0f; // animation phase

  SpellPickup() = default;
  explicit SpellPickup(Spell spell) : spell(spell) {}
};

} // namespace platformer
