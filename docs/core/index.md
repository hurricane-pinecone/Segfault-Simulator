# Engine core

`sfs::engine-core` is the dependency-free heart of the engine — no SDL, no OpenGL,
nothing platform-specific. It's the ECS, the particle engine, the Lua VM, and the
plain-data components, all usable on their own: a headless tool, a test, or a game
that brings its own rendering can link `engine-core` alone.

What lives here:

- **[Entities, components & systems](./ecs/index.md)** — the ECS data model and the
  GameObject layer over it.
- **[Particle engine](./particles/index.md)** — the particle simulation and how you
  author effects and decals (the simulation; *using* it in a rendered game is a
  runtime concern — see [Particles in your game](../runtime/particles/index.md)).
- **[Scripting](./scripting/index.md)** — the embedded Lua VM and the contracts for
  building a live modding API.

## Engine vs game ownership

**The game owns the world description and its content:** which entities exist and
their components, terrain, projection config, gameplay, input, and art.

**The engine owns the mechanics:** the ECS storage and scheduling, particle
simulation, world–screen projection types (`FlatProjection`,
`IsometricProjection`), and the scripting VM here in the core — and, in the
[runtime](../runtime/index.md), batching, lighting, shadows, and decals.

The game describes *what*; the engine runs *how*.
