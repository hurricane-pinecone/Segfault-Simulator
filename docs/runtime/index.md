# Runtime

`sfs::engine` is the full runtime: the [engine core](../core/index.md) plus the
SDL2 + OpenGL render and platform layer. It turns your entities and components into
a window, a frame loop, lit and shadowed rendering, input, assets, and an optional
dev console.

The runtime is structured in three nested layers, each a class you subclass and
whose hooks the engine calls for you:

```
Game     your application — window, the main loop, which scenes exist
  └─ Scene     a level or screen — owns the entities, systems, and game objects
       └─ System / GameObject     your gameplay and rendering logic
            └─ IQuadRenderer     the 2D draw API render systems draw through
```

- **[Game & scenes](./game-and-scenes/index.md)** — the `Game → Scene → System` lifecycle
  and the per-frame hooks where your code runs.
- **[Runtime systems](./systems/index.md)** — built-in systems for common mechanics
  such as camera follow.
- **[Rendering](./rendering/index.md)** — the render contract, the core/isometric
  backend seam, and how render features compose as modules.
- **[Particles in your game](./particles/index.md)** — registering the particle render
  module, spawning, and making decals stick (the simulation itself is
  [engine-core](../core/particles/index.md)).
- **[GPU voxel world](./voxel-gpu/index.md)** — the `engine-webgpu` brickmap
  simulation: WebGPU compute shaders, raymarched rendering, rigid body physics,
  and click editing.

The render systems draw in the frame's `render` phase through the injected
`IQuadRenderer`; everything else is your gameplay code in `update`. See
[Game & scenes](./game-and-scenes/index.md) for the full hook tables and the frame loop.
