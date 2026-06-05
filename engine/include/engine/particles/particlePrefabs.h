#pragma once

#include "engine/particles/particleEffect.h"
#include "glm/glm/ext/vector_float3.hpp"
#include <string>

namespace sfs
{

class ParticleEngine;

// Built-in particle effect prefabs. These are plain ParticleEffectDesc builders
// (the same struct a Lua/JSON loader fills), so a game can use them as-is,
// register them under its own names, or copy-and-tweak to extend them. They draw
// with the engine's built-in "white_dot" texture (a soft round white pixel), so
// they need no art -- colour does the work.

// --- Blood: a four-layer splatter meant to be fired together at one point ---
// A bright fast MIST haze, the main directional SPRAY of droplets, a few heavy
// GOBS that lag and splat, and DRIP that drops in place. spray/gobs/drip leave
// permanent decals where they land.
ParticleEffectDesc bloodMistEffect();
ParticleEffectDesc bloodSprayEffect();
ParticleEffectDesc bloodGobsEffect();
ParticleEffectDesc bloodDripEffect();

// --- Fire: a continuous additive ember drift, for an emitter on a light ---
ParticleEffectDesc emberEffect();

// Recolour a blood layer's gradient. The decals it leaves follow (they use the
// particle colour), so this is all a recoloured blood variant needs.
ParticleEffectDesc recolourBlood(ParticleEffectDesc desc, glm::vec3 hi, glm::vec3 lo);

// Register the four blood layers under "<prefix>_mist/_spray/_gobs/_drip" -- the
// names a shotgun-style spawn fires together. Default colours, or a recoloured
// set for a second blood colour.
void registerBloodEffects(ParticleEngine& particles,
                          const std::string& prefix = "blood");
void registerBloodEffects(ParticleEngine& particles,
                          const std::string& prefix,
                          glm::vec3 hi,
                          glm::vec3 lo);

} // namespace sfs
