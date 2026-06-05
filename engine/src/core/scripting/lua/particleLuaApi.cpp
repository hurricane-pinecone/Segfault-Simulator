#include "engine/core/scripting/particleLuaApi.h"

#include "engine/core/particles/particleEffect.h"
#include "engine/core/particles/particleEngine.h"
#include "engine/core/scripting/luaSchema.h"
#include "engine/core/scripting/luaScripting.h"
#include "engine/core/scripting/particleSchema.h"

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include <lua.hpp>

#include <exception>
#include <new>

namespace sfs
{

namespace
{

using Resolver = std::function<ParticleEngine*()>;
constexpr const char* kResolverMeta = "sfs.ParticleResolver";

// The resolver rides as every closure's shared upvalue (full userdata), so all
// five entry points reach the same live engine without a global.
ParticleEngine* engineOf(lua_State* L)
{
  auto* resolver = static_cast<Resolver*>(lua_touserdata(L, lua_upvalueindex(1)));
  return (resolver && *resolver) ? (*resolver)() : nullptr;
}

int resolverGc(lua_State* L)
{
  auto* resolver = static_cast<Resolver*>(lua_touserdata(L, 1));
  if (resolver)
    resolver->~Resolver(); // Lua frees the userdata memory; just run the dtor
  return 0;
}

// Convert a C++ exception from engine calls into a Lua error rather than letting
// it unwind through Lua's C frames (UB). The body unwinds before luaL_error
// longjmps.
template <typename Fn>
int guarded(lua_State* L, Fn&& body)
{
  try
  {
    return body();
  }
  catch (const std::exception& e)
  {
    return luaL_error(L, "%s", e.what());
  }
  catch (...)
  {
    return luaL_error(L, "unknown C++ exception in particles binding");
  }
}

int particlesSpawn(lua_State* L)
{
  return guarded(L,
                 [&]
                 {
                   const char* name = luaL_checkstring(L, 1);
                   const glm::vec2 pos{
                       static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                       static_cast<float>(luaL_optnumber(L, 3, 0.0))};
                   if (ParticleEngine* engine = engineOf(L))
                     engine->spawnBurst(
                         name, pos, engine->groundElevationAt(pos));
                   return 0;
                 });
}

// particles.configure(name, { color = sfs.colors.Green, burst = 30, sizeMin =,
//   ... }) -- every scalar key is driven by particleEffectSchema() via the
// reflection reader; only `color` (which maps onto a Gradient) is applied here.
int particlesConfigure(lua_State* L)
{
  return guarded(
      L,
      [&]
      {
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TTABLE);

        ParticleEngine* engine = engineOf(L);
        if (!engine)
          return 0;

        const ParticleEffectDesc* current = engine->effect(name);
        if (!current)
          return luaL_error(L, "unknown particle effect '%s'", name);

        ParticleEffectDesc desc = *current;
        luaschema::readTable(L, 2, &desc, particleEffectSchema());

        lua_getfield(L, 2, "color");
        if (lua_istable(L, -1))
        {
          const glm::vec3 c = luaschema::readColor(L, lua_gettop(L));
          desc.colorOverLife = Gradient::twoStop(c, c * 0.3f);
        }
        lua_pop(L, 1);

        engine->registerEffect(name, desc);
        return 0;
      });
}

// particles.describe(name) -> table of the effect's current configurable values
// (keyed exactly as configure reads them, so a dump round-trips as input).
int particlesDescribe(lua_State* L)
{
  return guarded(L,
                 [&]
                 {
                   const char* name = luaL_checkstring(L, 1);

                   ParticleEngine* engine = engineOf(L);
                   if (!engine)
                   {
                     lua_newtable(L);
                     return 1;
                   }

                   const ParticleEffectDesc* current = engine->effect(name);
                   if (!current)
                     return luaL_error(L, "unknown particle effect '%s'", name);

                   luaschema::pushValues(L, current, particleEffectSchema());
                   return 1;
                 });
}

// particles.effects() -> array of registered effect names (spawn/configure
// targets).
int particlesEffects(lua_State* L)
{
  return guarded(L,
                 [&]
                 {
                   lua_newtable(L);
                   if (ParticleEngine* engine = engineOf(L))
                   {
                     int i = 1;
                     for (const std::string& name : engine->effectNames())
                     {
                       lua_pushstring(L, name.c_str());
                       lua_rawseti(L, -2, i++);
                     }
                   }
                   return 1;
                 });
}

} // namespace

void registerParticleLua(LuaScripting& lua,
                         const std::string& tableName,
                         std::function<ParticleEngine*()> resolve)
{
  lua_State* L = lua.state();
  if (!L)
    return;

  // The resolver is owned by Lua as full userdata; its __gc runs the
  // std::function's destructor when the table (and the closures holding it) are
  // collected. Stack after this block: [..., resolver].
  void* mem = lua_newuserdatauv(L, sizeof(Resolver), 0);
  new (mem) Resolver(std::move(resolve));
  if (luaL_newmetatable(L, kResolverMeta))
  {
    lua_pushcfunction(L, &resolverGc);
    lua_setfield(L, -2, "__gc");
  }
  lua_setmetatable(L, -2);

  // Build the API table; every closure shares the one resolver userdata upvalue
  // (which sits just below the table at -2 throughout). Stack: [..., resolver, table].
  lua_newtable(L);

  const auto bindFn = [&](const char* field, lua_CFunction fn)
  {
    lua_pushvalue(L, -2); // copy the resolver userdata
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, field);
  };
  bindFn("spawn", &particlesSpawn);
  bindFn("configure", &particlesConfigure);
  bindFn("describe", &particlesDescribe);
  bindFn("effects", &particlesEffects);

  // particles.options: the configure schema (key -> hint), so it never drifts
  // from what configure reads.
  luaschema::pushSchema(L, particleEffectSchema());
  lua_setfield(L, -2, "options");

  lua_setglobal(L, tableName.c_str()); // pops the table
  lua_pop(L, 1);                        // pop the resolver (closures keep their ref)
}

} // namespace sfs
