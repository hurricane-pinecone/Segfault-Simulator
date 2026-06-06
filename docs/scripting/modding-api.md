# Modding API

How to build your game's Lua surface: implement one interface, compose bindings,
and reuse the building blocks.

## Implement `sfs::ILuaApi`

One interface, one method — the single entry point the host installs:

```cpp
#include <engine/core/scripting/iLuaApi.h> // also fwd-declares sfs::LuaScripting

class YourGame;

class GameLuaApi : public sfs::ILuaApi
{
public:
  explicit GameLuaApi(YourGame& game) : m_game(game) {}
  void registerBindings(sfs::LuaScripting& lua) override;
private:
  YourGame& m_game;
};
```

`registerBindings` is *thin* — it just composes your per-area binding modules:

```cpp
void GameLuaApi::registerBindings(sfs::LuaScripting& lua)
{
  registerWorldBindings(lua, m_game);
  registerActorBindings(lua, m_game);
}
```

### Per-area binding modules

Keep each slice of your API in its own small file (inline functions in your own
namespace). To add an API: drop in a `register<Area>Bindings(lua, game)` and add
one line to `registerBindings`.

```cpp
inline void registerActorBindings(sfs::LuaScripting& lua, YourGame& game)
{
  lua.bind("hurt", [&game](double n) { /* ... apply damage ... */ });
}
```

## Building blocks

Pick the lightest tool that fits.

### a) `lua.bind(name, fn)` — simple global functions

For plain triggers and number setters. Your game stays Lua-header-free.

```cpp
lua.bind("pause", [&game]                       { game.pause(); });       // void()
lua.bind("setHp", [&game](double v)             { game.player().hp = v; });// void(double)
lua.bind("warp",  [&game](double x, double y)   { game.warp(x, y); });    // void(double,double)
```

Only those three arities exist. For anything richer (tables, strings, return
values) use a config, an engine helper (below), or the raw Lua C API via
`lua.state()`.

### b) Resolve live game state *lazily*

Bindings outlive any single scene, so resolve through the game **at call time** —
never capture a scene or system pointer up front:

```cpp
sfs::ParticleEngine* particlesOf(sfs::Scene* scene)
{
  if (!scene || !scene->hasSystem<sfs::IsometricRenderSystem>())
    return nullptr;
  return scene->getSystem<sfs::IsometricRenderSystem>()
      .module<sfs::Particles<sfs::IsometricRenderContext>>();
}
// inside a bind: particlesOf(game.currentScene())  // re-resolved every call
```

### c) Reuse engine-provided tables

Some subsystems ship a ready-made table you just wrap. Particles is the example:

```cpp
sfs::registerParticleLua(
    lua, "particles",
    [&game] { return particlesOf(game.currentScene()); }); // lazy resolver
```

This installs `particles.spawn / configure / describe / effects / options`, driven
by the engine's particle schema. Your game only chooses the table name and how to
find the live engine. (Effect prefabs live engine-side too.)

## Engine scripting headers

`engine/core/scripting/`:

```
luaScripting.h     the VM: init / eval / evalRepl / bind / registerApi / registerConfig
iLuaApi.h          the contract your game implements (registerBindings)
iLuaConfigurable.h the contract for a live-editable object (get/set/options)
luaSchema.h        field() / rangeField() / colorField() + the reflection reader
particleLuaApi.h   registerParticleLua (the ready-made particle table)
```
