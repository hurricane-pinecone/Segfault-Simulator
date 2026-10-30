# Runtime

`sfs::engine` is the full runtime: the [engine core](../core/index.md) plus the
SDL2 platform layer, with the renderer in `sfs::engine-webgpu`. It turns your
entities and components into a window, a frame loop, lit and shadowed
rendering in any [style](./rendering/styles/index.md) (flat 2D, isometric 2D or
3D), input, assets, game UI and an optional dev console.

The runtime is structured in nested layers, each a class you subclass and
whose hooks the engine calls for you:

```
Game     your application — window, the main loop, which scenes exist
  └─ Scene     a level or screen — owns the entities, systems, and game objects
       └─ System / GameObject     your gameplay logic
       └─ RenderSystem + render modules     drawing, through the IRenderer
```

- **[Game & scenes](./game-and-scenes/index.md)**: the `Game → Scene → System`
  lifecycle and the per-frame hooks where your code runs.
- **[Scene files](./serialization/index.md)**: saving and loading scenes,
  their style included.
- **[Rendering](./rendering/index.md)**: the one renderer, the styles, render
  modules, sprites, meshes and lighting, and render backends.
- **[2D and 3D physics](./systems/index.md)**: movement and collision on a 2D
  game's plane, and [3D physics](./physics-3d/index.md) with Jolt.
- **[Runtime systems](./systems/index.md)**: built-in systems such as camera
  follow.
- **[Game UI](./ui/index.md)**: HUDs and menus, drawn over the scene.
- **[Particles in your game](./particles/index.md)**: the particle and decal
  modules, spawning, and making marks stick (the simulation itself is
  [engine-core](../core/particles/index.md)).
- **[Voxel world](./voxel/index.md)**: streaming voxel terrain with water.

The render system draws in the frame's `render` phase; everything else is your
gameplay code in `update`. See [Game & scenes](./game-and-scenes/index.md) for
the full hook tables and the frame loop.
