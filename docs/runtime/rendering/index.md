# Rendering

The engine has one renderer, and every game draws through it: a flat 2D
platformer, an isometric tile game and a free-camera 3D world are
[styles](./styles/index.md) of the same world, drawn by the same render
system with the same lights, shadows and effects.

```
Game
  └─ IRenderBackend     the window, the GPU device, the frame (WebGpuRenderBackend)
       └─ IRenderer     the draw API: meshes, materials, grass, UI (WebGpuRenderer)
            ▲
Scene       │
  └─ RenderSystem       camera, lighting, the style (Space2D), builds each frame
       └─ render modules    sprites, meshes, particles, decals, voxels, grass, UI
```

- **[Styles](./styles/index.md)**: how a flat, isometric or 3D game is set up,
  and `Space2D`, which places a 2D game's things in the world.
- **[Sprites & sprite sheets](./sprites/index.md)**: `SpriteComponent`,
  texture import settings and sprite-sheet frames.
- **[Render modules](./render-modules/index.md)**: the features a render
  system hosts, and writing your own.
- **[Meshes, materials & lighting](./meshes-and-lighting/index.md)**: meshes,
  materials, the camera, lights, shadows, bloom, depth of field, haze and grass.
- **[Render backends](./backends/index.md)**: `IRenderBackend`, the WebGPU
  backend, what each graphics API can do, and writing a backend of your own.

## One renderer, every platform

`IRenderer` (`engine/runtime/rendering/iRenderer.h`) is the draw API. Its one
implementation is `WebGpuRenderer`, provided by `WebGpuRenderBackend`
(`engine/webgpu/`), which runs on:

- **Desktop:** [wgpu-native](https://github.com/gfx-rs/wgpu-native), on
  Vulkan, Direct3D 12 or Metal, and on OpenGL where none of those exist.
- **The web:** the browser's WebGPU (emscripten's `emdawnwebgpu` port), in a
  recent Chrome, Edge or Safari, or Firefox 141+ on Windows.

Shaders are WGSL (`engine/src/shaders/*.wgsl`), the same on every platform.
Backends are pluggable: see [Render backends](./backends/index.md).

## Setting up a scene's rendering

A scene file (one made in the editor) is played by `StandardScene`, which sets
all of this up for you: a `RenderSystem` with the mesh, grass, sprite, decal,
particle and UI modules, in the style the file names.

A scene written in code adds a `RenderSystem` itself, with the backend's
renderer, and registers the modules it wants:

```cpp
#include "engine/runtime/rendering/modules/meshModule.h"
#include "engine/runtime/rendering/modules/spriteModule.h"
#include "engine/runtime/rendering/modules/uiModule.h"
#include "engine/runtime/rendering/renderSystem.h"

void MyScene::onInit()
{
  auto& render = addSystem<sfs::RenderSystem>(renderer());
  render.withModule<sfs::MeshModule>();
  render.withModule<sfs::SpriteModule>(assetStore());
  render.withModule<sfs::UiModule>(*this);
}
```

Without a `Space2D` that's a 3D game. A 2D game also gives the render system
its plane with `setSpace2D()`; see [Styles](./styles/index.md).

## Scene services

A scene reaches the backend's renderer and the asset store through the
`SceneServices` the backend supplies:

| Accessor | Returns |
| --- | --- |
| `renderer()` | The renderer. A fatal error if the backend has none. |
| `tryRenderer()` | The renderer, or null (for code that also runs headless). |
| `assetStore()` | The asset store: images, sprites, fonts, models, materials |

## A frame

Each frame, in the scene's render phase, `RenderSystem`:

1. Points its camera the way the scene's camera entity looks (the first
   active `CameraComponent`), unless told not to (`setFollowSceneCamera`).
2. Builds a `RenderContext`: the camera and its culling frustum, the sun's
   shadow volume, the lighting, the frame's point lights (from
   `PointLightComponent` entities and glowing materials), the mesh library,
   and the game's `Space2D` (null in a 3D game).
3. Lets every registered module emit its commands (`AnyRenderCommand`: mesh,
   grass and UI draws).
4. Sorts them by `RenderPass` (opaque first, then see-through, back to front)
   and submits them to the renderer, which draws shadows, the lit scene, bloom
   and depth of field, then the UI over the finished frame.

So registration order doesn't decide draw order, and a module never needs to
know about the others.

## Picking

`picking.h` finds what's under a ray (`RenderSystem::screenRay(pixel)` gives
the ray through a pixel): `pickEntity()` the nearest entity whose bounds it
enters (a sprite where it was drawn last frame, given the render system), and
`pickSurface()` the exact mesh triangle. The editor selects with these, in
every style.

## Stats and debug settings

`RenderSystem::stats()` reports the last frame's draw calls, instances,
shadow instances and point lights. `settings()` exposes lighting, shadow and
camera knobs as `ModuleSetting`s, so a debug panel shows them like any other
system's settings.
