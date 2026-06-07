# Render modules

Each render system composes its features as **render modules** rather than baking
them in. This is how a game enables the built-in rendering features and how it
adds its own.

## The model

A module is an `IRenderModule<TContext>` (usually a `CommandModule<TContext,
Command>`) that emits the frame's render commands for one feature. A render system
is a `RenderModuleHost<TContext>` that constructs, owns, and drives the modules it
is given — `TContext` is the render context the modules read (`IsometricRenderContext`
or `FlatRenderContext`), which keeps host and modules matched at compile time.

```cpp
// Register features on a render system. Order doesn't matter; registration enables.
renderSystem.withModules<sfs::TerrainShadow, sfs::SpriteShadow, sfs::BlockGeometry>();

auto& particles = renderSystem.withModule<sfs::Particles<Context>>();
```

## Registration is the enable

A feature is active **iff** its module type is registered. Adding the module turns
it on; removing it (`removeModule<T>()`) turns it off and drops its accumulated
state. There is no separate "enabled" flag to track.

Built-in modules include terrain shadows, sprite shadows, water, block geometry,
and particles (the isometric system); the flat system hosts particles the same
way. Decals are a module too, but the render system registers that one for you the
first time a feature asks for its decal sink — a game never registers it directly.

## Modules are independent

A module depends only on:

1. **The render context** — shared per-frame data such as the projection and the
   terrain elevation grid, plus cross-cutting flags (e.g. whether terrain is drawn
   as block faces).
2. **Host-provided capabilities** — e.g. the decal sink, reached through the host.

A module **never** references a sibling module. So features compose in any
combination, and a game can add its own module without touching the others. (For
the rule and its rationale, this is the engine-side counterpart of keeping systems
independent.)

## Dependencies are injected

The host hands each module its registry and asset store at registration, and
hands it the host itself, so a module **pulls** what it needs rather than wiring it
up by hand. A game registers a module and the host connects it.

## Writing your own module

Implement `IRenderModule<TContext>` (or extend `CommandModule<TContext, Command>`
to build one command type), then register it with `withModule<T>()`. Emit your
commands in the module's per-frame hook; they are ordered with everything else by
`RenderPass`.

## Built-in modules

The engine ships the following optional modules for the isometric render path.
Register whichever subset your scene needs; they compose in any order.

### BlockGeometry

`BlockGeometry` meshes every `IsometricTile`-tagged entity as a 3D block. Each
entity needs a `TransformComponent`, an `ElevationComponent`, and a
`SpriteComponent` that supplies the top-face texture. Adding a
`TerrainBoundaryComponent` exposes the south and east side faces where a block
drops to a lower neighbour.

Tiles that share a texture merge into one draw command. Tiles with different
textures produce separate commands. No geometry is emitted when the render
context carries no projection.

```cpp
renderSystem.withModule<sfs::BlockGeometry>();
```

### IsometricWater

`IsometricWater` renders water tiles. Each entity needs a `TransformComponent`,
an `ElevationComponent` for the terrain height beneath it, and a
`WaterTileComponent` that carries the water-surface elevation. Tiles where the
terrain is at or above the water surface are skipped. All visible water merges
into a single draw command.

```cpp
renderSystem.withModule<sfs::IsometricWater>();
```

### SpriteShadow

`SpriteShadow` casts a projected shadow for every entity that has a
`ShadowCasterComponent` and a resolvable `SpriteComponent`. The module emits
nothing when the render context supplies no light source (no ambient lighting
and no point lights).

```cpp
renderSystem.withModule<sfs::SpriteShadow>();
```

## Deferred: core passes as modules

The optional isometric features (shadows, water, block geometry, decals,
particles) are render modules, but the **core passes** — terrain tiles and sprites
— are still drawn directly by the render system. Some passes need cross-pass
orchestration (e.g. terrain shadows are skipped while block geometry is active),
so folding the core ones into modules is possible but not required.
