#include "scripting/luaBindings.h"

#include "effects/particleEffects.h"
#include "gameObjects/player.h"
#include "sampleGame.h"
#include "systems/TerrainGeneratorSystem.h"
#include <cmath>
#include <engine/components/transformComponent.h>
#include <engine/rendering/modules/particles.h>
#include <engine/sceneManager/scene.h>
#include <engine/scripting/luaScripting.h>
#include <engine/systems/isometric/isometricRenderSystem.h>
#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <lua.hpp>
#include <string>

namespace
{
using IsometricParticles = sfs::Particles<sfs::IsometricRenderContext>;

IsometricParticles* particlesOf(sfs::Scene* scene)
{
  if (!scene || !scene->hasSystem<sfs::IsometricRenderSystem>())
    return nullptr;
  return scene->getSystem<sfs::IsometricRenderSystem>()
      .module<IsometricParticles>();
}

float terrainElevationAt(sfs::Scene* scene, glm::vec2 pos)
{
  if (!scene || !scene->hasSystem<TerrainGeneratorSystem>())
    return 0.0f;
  return static_cast<float>(
      scene->getSystem<TerrainGeneratorSystem>().terrainHeightAt(
          static_cast<int>(std::floor(pos.x)),
          static_cast<int>(std::floor(pos.y))));
}

// Spray a gore blast at a world position via the active scene's particle
// module.
void spawnGoreAt(sfs::Scene& scene, glm::vec2 pos)
{
  IsometricParticles* particles = particlesOf(&scene);
  if (!particles)
    return;
  spawnGore(*particles,
            pos,
            terrainElevationAt(&scene, pos),
            glm::vec2{1.0f, 0.0f},
            12.0f);
}

// --- Lua `particles` table -------------------------------------------------
// Raw Lua C functions (they read tables/colours straight off the stack); the
// SampleGame* rides as the closure upvalue so they resolve the active scene at
// call time.

bool optNumberField(lua_State* L, int table, const char* key, double& out)
{
  lua_getfield(L, table, key);
  const bool ok = lua_isnumber(L, -1) != 0;
  if (ok)
    out = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return ok;
}

// Read an sfs.colors-style table ({r,g,b,a} 0-255, or {255,0,0}) into a 0..1
// vec3.
glm::vec3 readColor(lua_State* L, int idx)
{
  const auto channel = [&](const char* key, int arrayIndex) -> float
  {
    lua_getfield(L, idx, key);
    if (!lua_isnumber(L, -1))
    {
      lua_pop(L, 1);
      lua_rawgeti(L, idx, arrayIndex);
    }
    const float v = static_cast<float>(luaL_optnumber(L, -1, 0.0));
    lua_pop(L, 1);
    return v / 255.0f;
  };
  return glm::vec3{channel("r", 1), channel("g", 2), channel("b", 3)};
}

SampleGame* gameOf(lua_State* L)
{
  return static_cast<SampleGame*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int particlesSpawn(lua_State* L)
{
  const char* name = luaL_checkstring(L, 1);
  const glm::vec2 pos{static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                      static_cast<float>(luaL_optnumber(L, 3, 0.0))};

  sfs::Scene* scene = gameOf(L)->currentScene();
  if (IsometricParticles* particles = particlesOf(scene))
    particles->spawnBurst(name, pos, terrainElevationAt(scene, pos));
  return 0;
}

// particles.configure(name, { color = sfs.colors.Green, burst = 30, sizeMin =,
//   sizeMax =, speedMin =, speedMax =, lifetimeMin =, lifetimeMax =, gravityZ
//   =, drag =, spread = }) -- tweak a registered effect and re-apply it live.
int particlesConfigure(lua_State* L)
{
  const char* name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  IsometricParticles* particles = particlesOf(gameOf(L)->currentScene());
  if (!particles)
    return 0;

  const sfs::ParticleEffectDesc* current = particles->effect(name);
  if (!current)
    return luaL_error(L, "unknown particle effect '%s'", name);

  sfs::ParticleEffectDesc desc = *current;

  lua_getfield(L, 2, "color");
  if (lua_istable(L, -1))
  {
    const glm::vec3 c = readColor(L, lua_gettop(L));
    desc.colorOverLife = sfs::Gradient::twoStop(c, c * 0.3f);
  }
  lua_pop(L, 1);

  double v = 0.0;
  double lo = 0.0;
  double hi = 0.0;
  if (optNumberField(L, 2, "burst", v))
    desc.burstCount = static_cast<int>(v);
  if (optNumberField(L, 2, "spread", v))
    desc.directionSpread = static_cast<float>(v);
  if (optNumberField(L, 2, "gravityZ", v))
    desc.gravityZ = static_cast<float>(v);
  if (optNumberField(L, 2, "drag", v))
    desc.drag = static_cast<float>(v);
  if (optNumberField(L, 2, "sizeMin", lo) &&
      optNumberField(L, 2, "sizeMax", hi))
    desc.size =
        sfs::FloatRange::of(static_cast<float>(lo), static_cast<float>(hi));
  if (optNumberField(L, 2, "speedMin", lo) &&
      optNumberField(L, 2, "speedMax", hi))
    desc.speed =
        sfs::FloatRange::of(static_cast<float>(lo), static_cast<float>(hi));
  if (optNumberField(L, 2, "lifetimeMin", lo) &&
      optNumberField(L, 2, "lifetimeMax", hi))
    desc.lifetime =
        sfs::FloatRange::of(static_cast<float>(lo), static_cast<float>(hi));

  particles->registerEffect(name, desc);
  return 0;
}

// particles.effects() -> array of registered effect names (spawn/configure
// targets). Discovery for the Lua console.
int particlesEffects(lua_State* L)
{
  lua_newtable(L);
  if (IsometricParticles* particles = particlesOf(gameOf(L)->currentScene()))
  {
    int i = 1;
    for (const std::string& name : particles->effectNames())
    {
      lua_pushstring(L, name.c_str());
      lua_rawseti(L, -2, i++);
    }
  }
  return 1;
}

// Push `particles.options`: a table documenting the keys
// particles.configure(name, opts) understands (key -> type hint), so they're
// discoverable via autocomplete / the Fields box. The new table is left on top.
// KEEP IN SYNC with particlesConfigure above.
void pushConfigureOptions(lua_State* L)
{
  const auto opt = [&](const char* key, const char* hint)
  {
    lua_pushstring(L, hint);
    lua_setfield(L, -2, key);
  };

  lua_newtable(L);
  opt("color", "sfs.colors value (or {r,g,b})");
  opt("burst", "int: particles per blast");
  opt("spread", "radians: launch cone width");
  opt("gravityZ", "number: vertical accel");
  opt("drag", "number: velocity damping");
  opt("sizeMin", "tiles");
  opt("sizeMax", "tiles");
  opt("speedMin", "tiles/sec");
  opt("speedMax", "tiles/sec");
  opt("lifetimeMin", "seconds");
  opt("lifetimeMax", "seconds");
}
} // namespace

void registerGameLua(sfs::LuaScripting& lua, SampleGame& game)
{
  // Globals: splat() sprays at the player; spawnGore(x, y) at a world tile.
  // They resolve the scene/particle module lazily, so they work whatever scene
  // is up.
  lua.bind("splat",
           [&game]
           {
             sfs::Scene* scene = game.currentScene();
             if (!scene)
               return;
             if (Player* player = scene->tryFindObject<Player>())
               spawnGoreAt(*scene,
                           player->entity()
                               .getComponent<sfs::TransformComponent>()
                               .position);
           });

  lua.bind(
      "spawnGore",
      [&game](double x, double y)
      {
        if (sfs::Scene* scene = game.currentScene())
          spawnGoreAt(
              *scene, glm::vec2{static_cast<float>(x), static_cast<float>(y)});
      });

  // The `particles` table reads Lua tables (colours, config opts), which the
  // std::function binds can't, so push it with the raw Lua C API.
  if (lua_State* L = lua.state())
  {
    lua_newtable(L);

    lua_pushlightuserdata(L, &game);
    lua_pushcclosure(L, &particlesSpawn, 1);
    lua_setfield(L, -2, "spawn");

    lua_pushlightuserdata(L, &game);
    lua_pushcclosure(L, &particlesConfigure, 1);
    lua_setfield(L, -2, "configure");

    lua_pushlightuserdata(L, &game);
    lua_pushcclosure(L, &particlesEffects, 1);
    lua_setfield(L, -2, "effects");

    pushConfigureOptions(L); // particles.options (table)
    lua_setfield(L, -2, "options");

    lua_setglobal(L, "particles");
  }
}
