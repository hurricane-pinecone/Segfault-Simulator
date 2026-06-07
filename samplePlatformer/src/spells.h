#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace platformer
{

// Noita-style bullet modifiers. Picked up from enemy drops and stacked onto
// every shot; many can combine on one bolt (homing + bouncing + explosive ...).
enum class Spell
{
  Triple,    // +2 bolts in a spread per shot
  Bounce,    // bolts ricochet off platforms
  Homing,    // bolts curve toward the nearest enemy
  Pierce,    // bolts pass through enemies
  Explosive, // bolts detonate on impact (area damage)
  Chain,     // hits arc lightning to nearby enemies
  Gravity,   // bolts arc downward
  DamageUp,  // flat damage boost
  Rapid,     // faster fire rate
  Count
};

inline const char* spellName(Spell spell)
{
  switch (spell)
  {
  case Spell::Triple:
    return "TRIPLE";
  case Spell::Bounce:
    return "BOUNCE";
  case Spell::Homing:
    return "HOMING";
  case Spell::Pierce:
    return "PIERCE";
  case Spell::Explosive:
    return "EXPLOSIVE";
  case Spell::Chain:
    return "CHAIN";
  case Spell::Gravity:
    return "GRAVITY";
  case Spell::DamageUp:
    return "DAMAGE+";
  case Spell::Rapid:
    return "RAPID";
  default:
    return "?";
  }
}

// Vivid signature colour, used for the bolt light, the drop orb, and the
// pickup's aura.
inline glm::vec3 spellColor(Spell spell)
{
  switch (spell)
  {
  case Spell::Triple:
    return {1.0f, 0.9f, 0.2f};
  case Spell::Bounce:
    return {0.2f, 0.9f, 1.0f};
  case Spell::Homing:
    return {0.7f, 0.3f, 1.0f};
  case Spell::Pierce:
    return {1.0f, 1.0f, 1.0f};
  case Spell::Explosive:
    return {1.0f, 0.5f, 0.1f};
  case Spell::Chain:
    return {0.35f, 0.6f, 1.0f};
  case Spell::Gravity:
    return {0.3f, 1.0f, 0.4f};
  case Spell::DamageUp:
    return {1.0f, 0.3f, 0.6f};
  case Spell::Rapid:
    return {1.0f, 1.0f, 0.6f};
  default:
    return {1.0f, 1.0f, 1.0f};
  }
}

} // namespace platformer
