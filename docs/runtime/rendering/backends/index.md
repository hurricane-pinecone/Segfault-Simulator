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
each frame. It provides a 3D renderer (`WebGpuRenderer3D`) through
`SceneServices::renderer3D`, which a `RenderSystem3D` draws through (see
[3D rendering](../../rendering-3d/index.md)). Systems can also record their own
passes into the frame's encoder (`currentEncoder()`). It has no 2D quad or
text renderer.

It also hosts Dear ImGui with docking and multi-viewport enabled, so a debug or
tool panel can dock into the main window or be dragged out into its own OS
window. `imguiAvailable()` reports true for this backend.

The window opens hidden and shows once the first frame is drawn, so it never
sits black while the GPU is set up.

### Frame rate

By default it presents without waiting for the display (Mailbox, else
Immediate), so frame timings show what the GPU can do. To cap the frame rate at
the display's refresh rate (vsync), as the editor does, ask before the backend
initialises, in your game's `makeRenderBackend()`:

```cpp
std::unique_ptr<sfs::IRenderBackend> MyGame::makeRenderBackend()
{
  auto backend = std::make_unique<sfs::WebGpuRenderBackend>();
  backend->context().setVsync(true);
  return backend;
}
```

Panels dragged out into their own windows present without waiting: the main
window already paces the frame.

### Rendering the scene off-window

`setSceneTarget(WebGpuRenderTarget*)` redirects the 3D scene into an offscreen
colour texture instead of the window. The window is then cleared each frame and
only draws ImGui, which is the shape a tool needs to show the scene inside a
panel rather than filling the whole window. Pass `nullptr` to go back to drawing
the scene straight to the window.

```cpp
sfs::WebGpuRenderTarget viewport(webGpuContext);
viewport.resize({panelWidth, panelHeight}); // non-positive sizes release it

webGpuBackend->setSceneTarget(&viewport);
// ...
ImGui::Image(viewport.view(), ImVec2(panelWidth, panelHeight));
```

The target must outlive its use as the scene target, and `resize()` only
recreates the underlying texture when the size actually changes.

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

## Implementing a custom backend

Subclass `IRenderBackend`, implement all pure-virtual methods, and return an
instance from `makeRenderBackend()`. The backend must supply a valid
`SceneServices*` from `sceneServices()` before `onSetup()` runs, because
`Game::setup()` calls `sceneServices()` immediately after `makeRenderBackend()`
returns.
