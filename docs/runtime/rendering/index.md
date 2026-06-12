# Rendering

How a scene draws, and the seam between the core 2D renderer and the isometric
extension.

## The render contract

**A render system is any `sfs::System` that draws through the injected
`IQuadRenderer` between `begin()` and `flush()`.** `Scene::render()` calls
`render()` on every enabled system, so a scene "has a renderer" simply by adding
the render system it wants. Nothing forces a particular path.

- **Flat 2D game** → add `FlatRenderSystem` (lit sprites, point lights,
  particles) or `SpriteRenderSystem` (unlit sprites only), and feed a
  `FlatProjection` from the camera (`makeFlatProjection`).
- **Isometric game** → add `IsometricRenderSystem` and the isometric support
  systems.

### SpriteRenderSystem

`SpriteRenderSystem` draws every entity that has both a `TransformComponent` and
a `SpriteComponent`. It is the lightest render system: no lighting, no particles,
one textured quad per sprite.

**Anchors.** `SpriteComponent` takes an anchor point `{anchorX, anchorY}` in
normalised sprite coordinates. The anchor lands at the entity's world position.
An anchor of `{0.5, 1.0}` (centre-bottom) places the sprite so its bottom edge
sits at the transform position, which is the standard for ground-level actors.

```cpp
e.addComponent<SpriteComponent>(SpriteComponent{spriteId, {0.5f, 1.0f}});
```

**Camera offset.** Call `setCameraOffset` each frame with the camera's world
position so sprites scroll correctly. All quads are shifted by the negative of
the offset before submission.

```cpp
spriteRenderSystem.setCameraOffset(cameras.activeCamera().transform->position);
```

## The backend seam

The renderer is split into a core 2D contract and an isometric extension, so a
flat game never pays for heightfield machinery:

- **`IQuadRenderer`** (`iQuadRenderer.h`) is the core 2D contract — textures,
  quad / lit-quad / particle submission, lighting, and the frame lifecycle —
  implemented by `OpenGLQuadRenderer`.
- **`IIsometricRenderer : virtual IQuadRenderer`** (`iIsometricRenderer.h`) adds
  the heightfield surface: the elevation heightmap, block geometry, sun-shadow
  style, projected terrain/sprite shadows, and terrain stains (decals baked into paint textures). It is
  implemented by `IsometricGeometryRenderer`, a subclass of `OpenGLQuadRenderer`
  (`IQuadRenderer` is a virtual base, so there is one shared subobject).

`IsometricRenderSystem` requires the extension and obtains it from the injected
core renderer via `dynamic_cast`; a flat render system needs only the core.

### Choosing a backend

`Game::makeRenderBackend()` is the top-level factory the game overrides to choose
its graphics API. The default returns a `GLRenderBackend`. A game that renders
through WebGPU (for example, a brickmap voxel world driven by `VoxelGpuSystem`)
overrides it to return a `WebGpuRenderBackend` instead.

Within the OpenGL path, `Game::createQuadRenderer(width, height)` selects the 2D
renderer. It defaults to the flat-2D `OpenGLQuadRenderer`; an isometric game
overrides it to return an `IsometricGeometryRenderer`. The result is owned by the
`GLRenderBackend` and injected into every scene, reached by systems via
`Scene::quadRenderer()`.

See [Render backends](./backends/index.md) for the full `IRenderBackend` interface
and how to plug in a custom backend.

## OrthoOrbitCamera

`OrthoOrbitCamera` (`engine/runtime/rendering/camera/orthoOrbitCamera.h`) is a
math helper for a base-builder style view: orthographic projection locked to the
true-isometric pitch (~35.26°) with free yaw orbit around a focus point.

```cpp
sfs::OrthoOrbitCamera cam;
cam.focus  = {centreX, 0.0f, centreZ};
cam.yaw   += rotationDelta;           // orbit freely
cam.zoom   = 40.0f;                   // ortho half-height in world units
cam.aspect = static_cast<float>(w) / static_cast<float>(h);

glm::mat4 vp = cam.viewProj();        // feed to your render system each frame
```

`forward()` and `right()` return horizontal world-space vectors aligned to the
current yaw, so WASD movement stays camera-relative as the player orbits.

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

## Lighting and surface effects

Lighting and surface materials are driven by data, not shader code. Add a
`LightEmitterComponent` for a point light, configure ambient and sun lighting on
the render system, and tag tiles with a `SurfaceEffect` (grass, sand, water). The
renderer selects the matching lit shader for you, so a flat 2D game never carries
any isometric or heightfield shading.
