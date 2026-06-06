# Rendering

How a scene draws, and the seam between the core 2D renderer and the isometric
extension.

## The render contract

**A render system is any `sfs::System` that draws through the injected
`IQuadRenderer` between `begin()` and `flush()`.** `Scene::render()` calls
`render()` on every enabled system, so a scene "has a renderer" simply by adding
the render system it wants. Nothing forces a particular path.

- **Flat 2D game** → add `FlatRenderSystem` (lit sprites + point lights +
  particles) or `SpriteRenderSystem` (minimal unlit), and feed a `FlatProjection`
  from the camera (`makeFlatProjection`).
- **Isometric game** → add `IsometricRenderSystem` and the isometric support
  systems.

## The backend seam

The renderer is split into a core 2D contract and an isometric extension, so a
flat game never pays for heightfield machinery:

- **`IQuadRenderer`** (`iQuadRenderer.h`) is the core 2D contract — textures,
  quad / lit-quad / particle submission, lighting, and the frame lifecycle —
  implemented by `OpenGLQuadRenderer`.
- **`IIsometricRenderer : virtual IQuadRenderer`** (`iIsometricRenderer.h`) adds
  the heightfield surface: the elevation heightmap, block geometry, sun-shadow
  style, projected terrain/sprite shadows, and world-projected decals. It is
  implemented by `IsometricGeometryRenderer`, a subclass of `OpenGLQuadRenderer`
  (`IQuadRenderer` is a virtual base, so there is one shared subobject).

`IsometricRenderSystem` requires the extension and obtains it from the injected
core renderer via `dynamic_cast`; a flat render system needs only the core.

### Choosing a backend

`Game::createQuadRenderer(width, height)` is a virtual factory the game overrides
to pick its backend. It defaults to the flat-2D `OpenGLQuadRenderer`; an
isometric game overrides it to return an `IsometricGeometryRenderer`. The result
is owned by `Game` and injected into every `Scene`, reached by systems via
`Scene::quadRenderer()`.

## Projection

`IProjection` (`iProjection.h`) is the world↔screen transform a render system
depends on:

- `FlatProjection` — a panned / zoomed orthographic transform (`makeFlatProjection`).
- `IsometricProjection` — the heightfield transform (elevation as a Z offset).

The game feeds the active projection to its render system each frame from the
camera; the engine does the projection math from there.

## Isometric render settings

Isometric-only settings (e.g. sun-shadow sampling style) are configured through
`IsometricRenderSystem`, **not** the core `quadRenderer()`, so the core stays free
of isometric concepts.

## Command ordering

Each render module emits typed render commands for the frame; they are ordered
across the whole frame by `RenderPass` (and depth), so registration order doesn't
dictate draw order. World particles depth-test against the scene; screen-space
effects draw on top in the UI pass.

## Deferred: heightfield shader purity

`OpenGLQuadRenderer`'s lit shader still physically contains the heightfield
occlusion / sun-shadow / iso-top-mask GLSL and the heightmap state, even though it
is dormant for a flat game (the occlusion march early-returns when no heightmap is
uploaded). Relocating that GLSL into an `IsometricGeometryRenderer`-only shader
variant — so the base lit shader is literally plain 2D lighting — is deferred
because it needs visual verification that the isometric path stays pixel-identical.
