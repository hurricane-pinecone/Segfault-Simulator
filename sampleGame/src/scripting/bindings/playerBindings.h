#pragma once

#include "gameObjects/player.h"
#include "sampleGame.h"
#include "scripting/bindings/particleBindings.h" // particlesOf / spawnGoreAt
#include <engine/core/components/transformComponent.h>
#include <engine/core/scripting/luaScripting.h>

// Player Lua bindings (inline). registerPlayerBindings() wires the game's
// player-facing modding surface onto the VM.
namespace gamebindings
{

// splat() sprays a gore blast at the player's current position.
inline void registerPlayerBindings(sfs::LuaScripting& lua, SampleGame& game)
{
  lua.bind("splat",
           [&game]
           {
             sfs::ParticleEngine* particles = particlesOf(game.currentScene());
             if (!particles)
               return;
             if (Player* player = game.currentScene()->tryFindObject<Player>())
               spawnGoreAt(*particles,
                           player->entity()
                               .getComponent<sfs::TransformComponent>()
                               .position);
           });
}

} // namespace gamebindings
