# Engine core

`sfs::engine-core` is the dependency-free heart of the engine: no SDL, no GPU,
nothing platform-specific. It's the ECS, the particle engine, the Lua VM, the
asset database, the plain-data components, and the data the renderer draws
(meshes, materials, lighting), all usable on their own: a headless tool, a
test, or a game that brings its own rendering can link `engine-core` alone.

What lives here:

- **[Entities, components & systems](./ecs/index.md)**: the ECS data model and
  the GameObject layer over it.
- **[Reflection & codegen](./reflection/index.md)**: how components describe
  their fields, for the editor and scene files.
- **[Parents & children](./hierarchy/index.md)**: the entity hierarchy.
- **[Asset database](./assets/index.md)**: the project's asset files and their
  ids and import settings.
- **[Rendering data](./rendering/index.md)**: meshes, materials, material
  graphs, the camera and lighting settings, with no GPU.
- **[Voxel terrain](./voxel/index.md)**: block types, chunks, the mesher and
  the water simulation.
- **[Particle engine](./particles/index.md)**: the particle simulation and how
  you author effects and decals (the simulation; *using* it in a rendered game
  is a runtime concern, see [Particles in your game](../runtime/particles/index.md)).
- **[Scripting](./scripting/index.md)**: the embedded Lua VM and the contracts
  for building a live modding API.

## Engine vs game ownership

**The game owns the world description and its content:** which entities exist
and their components, its style (flat, isometric or 3D) and terrain, gameplay,
input, and art.

**The engine owns the mechanics:** the ECS storage and scheduling, particle
simulation, the scripting VM and the rendering data here in the core, and, in
the [runtime](../runtime/index.md), placing a 2D game's things in the world,
batching, lighting, shadows, decals and drawing.

The game describes *what*; the engine runs *how*.
