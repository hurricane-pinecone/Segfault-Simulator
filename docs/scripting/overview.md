# Scripting

Give a game built on **sfs** a live Lua modding API: editable at runtime, no
rebuild, working on both native and web. The engine provides the VM and reusable
building blocks; **your game decides what to expose** as its modding surface.

Deeper pages:

- [Modding API](./modding-api.md) — implement `ILuaApi`, compose bindings, and the
  building blocks (`bind`, lazy resolution, engine-provided tables).
- [Live config](./live-config.md) — make a class live-editable with
  `ILuaConfigurable` and a field schema, no Lua code required.
- [Runtime & safety](./runtime.md) — the in-app console, the web editor, driving
  eval yourself, the sandbox, and lifetimes.

## The layering

| Layer | Owns | Examples |
| --- | --- | --- |
| **Engine** (`sfs::`) | the VM + reusable building blocks | `LuaScripting`, `ILuaApi`, `ILuaConfigurable`, `registerParticleLua` |
| **Game** (your code) | a curated *modding API* built from those blocks | your `ILuaApi` implementation and its bindings |

The engine never decides your game's API. You implement one interface
(`sfs::ILuaApi`) and compose the surface from the building blocks.

## Lifecycle

- **Startup** (app level) — create the `LuaScripting` VM, `init()` it, mark it
  active, and install your API once. The VM persists for the whole app.
- **Before the first scene** — the VM must exist before any scene's `onInit`
  registers config against it (see [live config](./live-config.md)).
- **Runtime** — re-running a chunk against the *persistent* VM mutates the live
  game; bindings resolve the current scene at call time, so they survive scene
  changes.
- **Teardown** — clear the active VM, then destroy it.

## Quick start

Own the VM at the application level (it must outlive scenes) and install your API:

```cpp
// In your Game subclass:
void YourGame::setupLua()
{
  m_lua = std::make_unique<sfs::LuaScripting>();
  m_lua->init();
  sfs::setActiveLua(m_lua.get()); // routes the web editor's eval here
  m_lua->setConsoleEnabled(true); // optional in-app console (backtick)

  GameLuaApi api(*this);          // your ILuaApi; transient
  m_lua->registerApi(api);        // calls api.registerBindings(*m_lua)
}

void YourGame::onDestroy()
{
  sfs::setActiveLua(nullptr);     // the VM is destroyed after this
}
```

> **Ordering:** call `setupLua()` **before** creating the first scene — a scene's
> `onInit` runs synchronously and may register config against the VM.

Keep `setupLua` minimal — *only* VM lifecycle. The actual API surface lives in
your `ILuaApi`; see [Modding API](./modding-api.md).
