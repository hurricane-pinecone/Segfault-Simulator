# Architecture

## What this engine is

SegFaultSimulator is a **2D game engine** with an ECS core and two render paths
over one core renderer:

- **Flat 2D** — screen-space sprites with per-pixel point lighting, normal maps,
  and particles, for side-on or top-down games.
- **Isometric heightfield** — the world is a **2D grid of columns, each with a
  single elevation** (a heightfield), drawn as billboard sprites or extruded
  block-face geometry and lit/shadowed with the elevation as a Z coordinate.

The two paths share the core `IQuadRenderer` (quads, lit quads, particles, point
lights); the isometric heightfield work (elevation occlusion, terrain/sprite
shadows, block geometry, world-projected decals) is the `IIsometricRenderer`
extension on top. A game picks a path by adding the matching render system; the
core forces neither.

What it is **not**: a general 3D engine. There are no arbitrary meshes, no
overhangs (a heightfield column has one height), no free camera, and no
perspective. The isometric "3D" look is a heightfield projected isometrically and
lit per elevation. See *Seams* below.

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
- **Render systems**: ordinary `System`s that draw through the renderer.
  `IsometricRenderSystem` is the heightfield one; `FlatRenderSystem` is the lit
  flat-2D one (sprites + point lights + module-hosted particles, ordered by
  layer then Y); `SpriteRenderSystem` is a minimal unlit flat-2D one. The flat
  systems need only the core `IQuadRenderer`; the isometric one requires the
  `IIsometricRenderer` extension.

## The render contract

**A render system is any `sfs::System` that draws through the injected
`IQuadRenderer` between `begin()` and `flush()`.** `Scene::render()` calls
`render()` on every enabled system, so a scene "has a renderer" simply by adding
the render system it wants. Nothing forces the isometric path.

- Flat 2D game → add `FlatRenderSystem` (lit sprites + point lights + particles)
  or `SpriteRenderSystem` (minimal unlit), and feed a `FlatProjection` from the
  camera (`makeFlatProjection`).
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
  render system depends on. `FlatProjection` (panned/zoomed orthographic) and
  `IsometricProjection` (heightfield) implement it.
- **Custom render passes.** A game can inject its own render pass by implementing
  `IRenderProvider` and registering it with
  `IsometricRenderSystem::addRenderProvider`; its commands are ordered with the
  built-in passes by `RenderPass`.
- **Isometric render settings** (e.g. sun-shadow style) are set through
  `IsometricRenderSystem`, not the core `quadRenderer()`, so the core stays free
  of iso concepts.

## Deferred / future work

These are intentionally not built yet, to avoid abstraction without a consumer:

- **Heightfield shader purity.** `OpenGLQuadRenderer`'s lit shader still
  physically contains the heightfield occlusion / sun-shadow / iso-top-mask GLSL
  and the heightmap state, even though it is dormant for a flat game (the
  occlusion march early-returns when no heightmap is uploaded). Relocating that
  GLSL into an `IsometricGeometryRenderer`-only shader variant — so the base lit
  shader is literally plain 2D lighting — is deferred because it needs visual
  verification that the isometric path stays pixel-identical.
- **Built-in passes via the registration API.** The built-in iso passes are pulled
  explicitly by the render system (some need cross-pass orchestration, e.g. the
  terrain shadow pass is skipped when block geometry is active). Migrating them
  onto `IRenderProvider` is possible but not required.
