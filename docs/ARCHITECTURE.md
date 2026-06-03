# Architecture

## What this engine is

SegFaultSimulator is a **2.5D isometric heightfield engine** with an ECS core. The
defining assumption runs through every rendering subsystem: the world is a **2D
grid of columns, each with a single elevation** (a heightfield). That heightfield
can be drawn two ways — cheap billboard sprites or extruded block-face geometry —
and lit/shadowed with the elevation as a Z coordinate.

What it is **not**: a general 3D engine. There are no arbitrary meshes, no
overhangs (a column has one height), no free camera, and no perspective. The
"3D" look is a heightfield projected isometrically and lit per elevation.

The engine is built so the isometric path is **one renderer behind a seam**, not
the whole engine — a flat 2D path (and, later, 3D) can be added without changing
the core. See *Seams* below.

## Layering

```
Game (your subclass)         window/config; createQuadRenderer() picks the backend
  └─ Scene                   a bag of Systems; render() calls every system's render()
       └─ System(s)          gameplay + rendering live here as ECS systems
            └─ IQuadRenderer core 2D draw API the render systems draw through
                 ├─ OpenGLQuadRenderer          core 2D OpenGL backend (default)
                 └─ IsometricGeometryRenderer   iso subclass (adds IIsometricRenderer)
```

- **ECS core** (`engine/ecs`): `Entity`, `Registry`, `System`. Generic.
- **Backend** (`IQuadRenderer`): backend-agnostic 2D primitives. Created by the
  game via `Game::createQuadRenderer` (defaults to the flat-2D `OpenGLQuadRenderer`;
  the iso sample overrides it to `IsometricGeometryRenderer`), owned by `Game`,
  injected into every `Scene`, reached by systems via `Scene::quadRenderer()`.
- **Backends**: `OpenGLQuadRenderer` implements the core `IQuadRenderer`.
  `IsometricGeometryRenderer : OpenGLQuadRenderer, IIsometricRenderer` adds the
  heightfield pipelines (`IQuadRenderer` is a virtual base, so there's one shared
  subobject).
- **Render systems**: ordinary `System`s that draw through the renderer. The
  isometric renderer (`IsometricRenderSystem`) is one; `SpriteRenderSystem` is a
  flat-2D one.

## The render contract

**A render system is any `sfs::System` that draws through the injected
`IQuadRenderer` between `begin()` and `flush()`.** `Scene::render()` calls
`render()` on every enabled system, so a scene "has a renderer" simply by adding
the render system it wants. Nothing forces the isometric path.

- Flat 2D game → add `SpriteRenderSystem` (`engine/systems/spriteRenderSystem.h`).
- Isometric game → add `IsometricRenderSystem` and the iso support systems.

## Engine vs game ownership

**The game owns the world description and configuration:**
- Terrain: which tiles exist and their elevations (e.g. `TerrainGeneratorSystem`).
- Per-tile content: sprite, shape, surface effect, water, light emitters.
- Projection config (tile size, elevation step, scale) and the system list.
- Gameplay, input, actors, art.

**The engine owns how that world is rendered:**
- Projection math, batching, billboard-vs-geometry, sun + point-light lighting,
  terrain occlusion, shadows, decals, particles.

The game never does projection or lighting math; it tags entities with components
and the engine renders them.

## Seams (extension points)

- **Backend selection.** `Game::createQuadRenderer(w, h)` is a virtual factory
  the game overrides to choose its backend. It defaults to the flat-2D
  `OpenGLQuadRenderer`; the iso sample overrides it to `IsometricGeometryRenderer`.
- **Core vs isometric renderer.** `IQuadRenderer` (`iQuadRenderer.h`) is the core
  2D contract (textures, quad/lit/particle submission, lighting, frame
  lifecycle), implemented by `OpenGLQuadRenderer`. `IIsometricRenderer : virtual
  IQuadRenderer` (`iIsometricRenderer.h`) adds the heightfield surface (elevation
  heightmap, block geometry, sun-shadow style, projected terrain/sprite shadows,
  world-projected decals), implemented by `IsometricGeometryRenderer` (a subclass
  of `OpenGLQuadRenderer`). `IsometricRenderSystem` requires the extension (it
  obtains it from the injected core renderer via `dynamic_cast`); a flat-2D render
  system needs only the core. The core's lit pipeline carries the heightmap as
  optional occlusion infra — inert until an iso backend uploads one.
- **Projection.** `IProjection` (`iProjection.h`) is the world↔screen transform a
  render system depends on. `IsometricProjection` implements it.
- **Custom render passes.** A game can inject its own render pass by implementing
  `IRenderProvider` and registering it with
  `IsometricRenderSystem::addRenderProvider`; its commands are ordered with the
  built-in passes by `RenderPass`.
- **Isometric render settings** (e.g. sun-shadow style) are set through
  `IsometricRenderSystem`, not the core `quadRenderer()`, so the core stays free
  of iso concepts.

## Deferred / future work

These are intentionally not built yet, to avoid abstraction without a consumer:

- **Projection through the context.** `IsometricRenderContext` holds a concrete
  `IsometricProjection` because the iso systems use iso-specific fields directly;
  routing render systems through `IProjection` generically waits on a non-iso
  consumer (mainly relevant to a future 3D path).
- **Built-in passes via the registration API.** The built-in iso passes are pulled
  explicitly by the render system (some need cross-pass orchestration, e.g. the
  terrain shadow pass is skipped when block geometry is active). Migrating them
  onto `IRenderProvider` is possible but not required.
- **Shader decoupling.** The lit/decal shaders bake some isometric projection
  math; a non-iso lit path would parameterize or replace them.
