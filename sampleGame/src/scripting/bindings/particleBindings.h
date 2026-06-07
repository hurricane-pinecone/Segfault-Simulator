#pragma once

#include "effects/particleEffects.h"
#include "sampleGame.h"
#include <engine/core/scripting/luaScripting.h>
#include <engine/core/scripting/particleLuaApi.h>
#include <engine/runtime/rendering/modules/particles.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/systems/isometric/isometricRenderSystem.h>
#include <glm/glm/ext/vector_float2.hpp>

// Particle Lua bindings (inline). registerParticleBindings() wires the game's
// particle modding surface onto the VM; the helpers are shared with other
// binding modules (e.g. the player's splat).
namespace gamebindings
{

using IsometricParticles = sfs::Particles<sfs::IsometricRenderContext>;

// The live particle engine for a scene, or nullptr if the scene isn't up / has
// no render system. Returned as the base ParticleEngine -- all the modding API
// (engine particle table, spawnGore) works on that.
inline sfs::ParticleEngine* particlesOf(sfs::Scene* scene)
{
  if (!scene || !scene->hasSystem<sfs::IsometricRenderSystem>())
    return nullptr;
  return scene->getSystem<sfs::IsometricRenderSystem>()
      .module<IsometricParticles>();
}

// Spray a gore blast at a world position, on the terrain there.
inline void spawnGoreAt(sfs::ParticleEngine& particles, glm::vec2 pos)
{
  spawnGore(particles,
            pos,
            particles.groundElevationAt(pos),
            glm::vec2{1.0f, 0.0f},
            12.0f);
}

// spawnGore(x, y) global + the engine's base particle table (spawn / configure
// / describe / effects / options). Both resolve the live engine lazily, so they
// work whatever scene is up.
inline void registerParticleBindings(sfs::LuaScripting& lua, SampleGame& game)
{
  lua.bind(
      "spawnGore",
      [&game](double x, double y)
      {
        if (sfs::ParticleEngine* particles = particlesOf(game.currentScene()))
          spawnGoreAt(*particles,
                      glm::vec2{static_cast<float>(x), static_cast<float>(y)});
      });

  sfs::registerParticleLua(
      lua, "particles", [&game] { return particlesOf(game.currentScene()); });
}

} // namespace gamebindings
