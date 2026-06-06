# Architecture

A high-level map of how the engine fits together. Deeper pages:

- [Rendering](./rendering.md) — the render contract, the core/isometric backend
  seam, projection, and render settings.
- [Render modules](./render-modules.md) — how render systems compose their
  features as pluggable modules.

## What this engine is

SegFaultSimulator is a **2D game engine** with an ECS core and two render paths
over one core renderer:

- **Flat 2D** — screen-space sprites with per-pixel point lighting, normal maps,
  and particles, for side-on or top-down games.
- **Isometric heightfield** — the world is a **2D grid of columns, each with a
  single elevation** (a heightfield), drawn as billboard sprites or extruded
  block-face geometry and lit/shadowed with the elevation as a Z coordinate.

The two paths share the core `IQuadRenderer` (quads, lit quads, particles, point
lights); the isometric work (elevation occlusion, terrain/sprite shadows, block
geometry, world-projected decals) is the `IIsometricRenderer` extension on top. A
game picks a path by adding the matching render system; the core forces neither.

What it is **not**: a general 3D engine. There are no arbitrary meshes, no
overhangs (a heightfield column has one height), no free camera, and no
perspective. The isometric "3D" look is a heightfield projected isometrically and
lit per elevation.

## Layering

```
Game (your subclass)         window/config; createQuadRenderer() picks the backend
  └─ Scene                   a bag of Systems; render() calls every system's render()
       └─ System(s)          gameplay + rendering live here as ECS systems
            └─ IQuadRenderer core 2D draw API the render systems draw through
                 ├─ OpenGLQuadRenderer          core 2D OpenGL backend (default)
                 └─ IsometricGeometryRenderer   iso subclass (adds IIsometricRenderer)
```

- **ECS core** (`engine/core/ecs`): `Entity`, `Registry`, `System`. Generic.
- **Renderer backend** (`IQuadRenderer`): backend-agnostic 2D primitives, created
  by the game, owned by `Game`, injected into every `Scene`.
- **Render systems**: ordinary `System`s that draw through the renderer —
  `IsometricRenderSystem` (heightfield), `FlatRenderSystem` (lit flat 2D), and
  `SpriteRenderSystem` (minimal unlit flat 2D).

See [Rendering](./rendering.md) for the backend seam and the render contract.

## Composing a render system

A render system isn't a monolith: its optional features — shadows, water, block
geometry, decals, particles — are **render modules** registered onto it, and
*registration is the enable*. A game adds only the features it wants and can write
its own. See [Render modules](./render-modules.md).

## Engine vs game ownership

**The game owns the world description and configuration:**

- Terrain: which tiles exist and their elevations.
- Per-tile content: sprite, shape, surface effect, water, light emitters.
- Projection config (tile size, elevation step, scale) and the system list.
- Gameplay, input, actors, art.

**The engine owns how that world is rendered:**

- Projection math, batching, billboard-vs-geometry, sun + point-light lighting,
  terrain occlusion, shadows, decals, particles.

The game never does projection or lighting math; it tags entities with components
and the engine renders them.
