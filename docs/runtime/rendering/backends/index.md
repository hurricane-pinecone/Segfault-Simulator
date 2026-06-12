# Render backends

A render backend owns the window, the GPU device or context, and the per-frame
lifecycle (begin frame, end frame). `Game` picks one by overriding
`makeRenderBackend()`. Scenes never touch the backend directly — they draw through
the `SceneServices` the backend provides.

## IRenderBackend

`IRenderBackend` (`engine/runtime/rendering/backend/iRenderBackend.h`) is the
interface every backend implements:

| Method | Purpose |
| --- | --- |
| `init(title, w, h)` | Create the window and GPU device. Returns false on failure. |
| `window()` | The SDL window, for event routing in `Game`. |
| `onResize(w, h)` | Called when the window changes size. |
| `sceneServices()` | Returns the drawing/asset bundle scenes are constructed against. |
| `beginFrame(w, h)` | Acquire the backbuffer and clear it. |
| `endFrame()` | Submit and present. |
| `shutdown()` | Destroy all GPU resources while the context is still valid. |

ImGui integration is optional. `imguiAvailable()` returns false on a backend that
does not have it, and the debug overlay in `Game` is suppressed automatically.

## GLRenderBackend

`GLRenderBackend` (`engine/runtime/rendering/backend/glRenderBackend.h`) is the
default backend. It creates an SDL OpenGL window and context, initialises the quad
renderer and asset store, sets up ImGui on native builds, and fills the
`SceneServices` bundle that scenes receive.

You do not construct it directly. `Game::makeRenderBackend()` builds it, passing
`createQuadRenderer` as the factory for the 2D renderer. Override
`createQuadRenderer(w, h)` to change which renderer the backend uses:

- Default: `OpenGLQuadRenderer` (flat 2D).
- Isometric games: `IsometricGeometryRenderer`.

## WebGpuRenderBackend

`WebGpuRenderBackend` (`engine/webgpu/webGpuRenderBackend.h`) opens an SDL window
without a GL context, initialises a WebGPU device and surface through
`WebGpuContext`, and opens a command encoder plus backbuffer view at the start of
each frame. It does not draw anything itself — a `VoxelGpuSystem` records compute
and raymarch passes into that encoder.

To use it, link `sfs::engine-webgpu` and override `makeRenderBackend()`:

```cpp
#include "engine/webgpu/webGpuRenderBackend.h"

std::unique_ptr<sfs::IRenderBackend> MyGame::makeRenderBackend()
{
  return std::make_unique<sfs::WebGpuRenderBackend>();
}
```

Retrieve the typed backend when constructing systems that need the device:

```cpp
auto* wgpu = static_cast<sfs::WebGpuRenderBackend*>(renderBackend());
```

Read more: [GPU voxel world](../../voxel-gpu/index.md).

## Implementing a custom backend

Subclass `IRenderBackend`, implement all pure-virtual methods, and return an
instance from `makeRenderBackend()`. The backend must supply a valid
`SceneServices*` from `sceneServices()` before `onSetup()` runs, because
`Game::setup()` calls `sceneServices()` immediately after `makeRenderBackend()`
returns.
