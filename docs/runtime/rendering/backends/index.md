# Render backends

A render backend owns the window, the GPU device and the per-frame lifecycle
(begin frame, end frame), and provides the renderer scenes draw through.
Scenes never touch the backend directly: they draw through the
`SceneServices` it provides (`renderer()`, `assetStore()`).

The engine has one backend, `WebGpuRenderBackend`, and it runs everywhere the
engine does. Backends are pluggable, so a program can add its own.

## Choosing a backend

Backends are offered by name through the `RenderBackends` registry
(`engine/runtime/rendering/backend/renderBackends.h`):

| Call | Does |
| --- | --- |
| `RenderBackends::add(name, factory)` | Offers a backend (replacing any of the same name) |
| `RenderBackends::make(name)` | A new backend of that name, or null |
| `RenderBackends::names()` | Every backend's name, in the order added |

`registerWebGpuBackend()` (`engine/webgpu/webGpuRenderBackend.h`) offers the
engine's, as `"webgpu"`. The editor and the exported-game player call it at
startup.

`Game::makeRenderBackend()` creates the backend a game runs on. By default
it's the first one offered; a game can override it to return any backend,
its own included:

```cpp
#include "engine/webgpu/webGpuRenderBackend.h"

std::unique_ptr<sfs::IRenderBackend> MyGame::makeRenderBackend()
{
  return std::make_unique<sfs::WebGpuRenderBackend>();
}
```

If the backend can't start (no driver for it on this machine), `Game` tries
the others offered, in order, and logs which one it renders with. An exported
game's `game.json` can name the backend it prefers (`GameManifest::backend`);
empty means the player's first.

`renderBackend()` returns the active backend, for code that needs its
concrete type.

## IRenderBackend

`IRenderBackend` (`engine/runtime/rendering/backend/iRenderBackend.h`) is the
interface every backend implements:

| Method | Purpose |
| --- | --- |
| `name()` | Its name among `RenderBackends`, for logs and settings |
| `init(title, w, h)` | Create the window and GPU device. Returns false (and logs) on failure. |
| `window()` | The SDL window, for event routing in `Game` |
| `onResize(w, h)` | Called when the window changes size |
| `sceneServices()` | The renderer and asset store scenes are constructed with |
| `beginFrame(w, h)` / `endFrame()` | Acquire and clear the backbuffer; submit and present |
| `setVsync(on)` | Wait for the display when presenting, before `init()` or any time after. |
| `deviceLost()` | Whether the GPU device was lost; `Game` ends the game (with a message) if so. |
| `shutdown()` | Destroy all GPU resources while the device is still valid |

ImGui integration is optional: `imguiProcessEvent`, `imguiNewFrame`,
`imguiRenderDrawData`, and `imguiAvailable()`, which gates the debug overlay
in `Game`.

### For tools

The editor talks to the GPU only through these, so it runs on any backend
that has them. A backend without them returns null, and a tool shows less (no
viewport panel, previews or thumbnails).

| Method | Purpose |
| --- | --- |
| `createRenderTarget()` | An offscreen picture (`IRenderTarget`, `renderTarget.h`) to draw into and show in an ImGui panel |
| `setSceneTarget(target)` | Draw the scene into that target instead of the window, which then only shows ImGui; null goes back to the window |
| `createRenderer(shadowMapResolution)` | Another `IRenderer` on the same device, for previews drawn apart from the scene |
| `drawInto(target, renderer, draw)` | Draw one frame of such a renderer into a target |
| `createImage(size, rgba)` | A fixed picture (`IImage`) from RGBA pixels, such as an asset's thumbnail |

An `IRenderTarget` is shown with
`ImGui::Image(ImTextureID(target.imguiTexture()), ...)`. `resize(size)` only
remakes it when the size changes; a size of 0 frees it.

## WebGpuRenderBackend

`WebGpuRenderBackend` (`engine/webgpu/webGpuRenderBackend.h`, in the
`sfs::engine-webgpu` library) opens an SDL window with a WebGPU device and
surface (`WebGpuContext`), and provides `WebGpuRenderer` as the scenes'
renderer. It runs on:

- **Desktop:** wgpu-native, which picks Vulkan, Direct3D 12 or Metal, and
  OpenGL on machines without any of them. Set `SFS_WGPU_BACKEND` to `vulkan`,
  `dx12`, `metal` or `gl` to choose one, for example to try the OpenGL
  fallback.
- **The web:** the browser's WebGPU, through emscripten's `emdawnwebgpu` port
  (the same `webgpu.h`). It needs a recent Chrome, Edge or Safari, or Firefox
  141+ on Windows.

It hosts Dear ImGui with docking and multi-viewport, so a debug or tool panel
can dock into the main window or be dragged out into its own OS window. The
window opens hidden and shows once the first frame is drawn, so it never sits
black while the GPU is set up.

By default it presents without waiting for the display (Mailbox, else
Immediate), so frame timings show what the GPU can do. `setVsync(true)` caps
the frame rate at the display's refresh rate, as the editor and an exported
game (unless its `game.json` says `"vsync": false`) do; `Game::setVsync`
changes it while the game runs, and `Game::setMaxFrameRate` (`"maxFrameRate"`
in `game.json`) caps it further. The surface is sized in pixels, which on a
high-DPI screen is more than the window's size in points; mouse positions are
given in the same pixels.

`SFS_SCREENSHOT=<file.png>` draws the scene offscreen and saves frame
`SFS_SCREENSHOT_FRAME` (60 by default) there, for checking how a game looks
without seeing its window.

### What each graphics API can do

`IRenderer::capabilities()` says what a renderer can draw
(`RendererCapabilities`). Render modules and the editor skip what it lacks.
On Vulkan, Direct3D 12, Metal and the browser's WebGPU, everything is on. On
wgpu's OpenGL fallback, these are off:

- multisampled anti-aliasing (`antiAliasing`)
- depth of field (`depthOfField`)
- point-light shadows (`pointLightShadows`)

## The renderer contract

`IRenderer` (`engine/runtime/rendering/iRenderer.h`) is what `RenderSystem`
draws through, for flat, isometric and 3D games alike:

| Call | Does |
| --- | --- |
| `createMesh` / `destroyMesh` | Upload or free a mesh (`MeshHandle`) |
| `createMaterial` / `updateMaterial` / `destroyMaterial` | Upload, change or free a material and its textures (`MaterialHandle`) |
| `createGrass`, `updateGrassGround`, `updateGrassMaps`, `updateGrassTrample`, `destroyGrass` | A grass field's ground and maps |
| `beginFrame(FrameParams)` | Start a frame: camera, lighting, shadows, wind, time |
| `drawMesh(MeshBatch)` | Queue one mesh's instances (lit, shadow-casting) |
| `drawGrass(GrassBatch)` | Queue a grass field |
| `drawUi(UiDrawList)` | Queue UI to draw over the finished frame |
| `endFrame()` | Render everything queued: shadow maps, the lit scene, bloom and depth of field, then the UI |
| `capabilities()`, `viewportSize()`, `shadowMapResolution()` | What it can do, and how big its target is |

## Writing a backend

A backend for another graphics API implements two interfaces: an
`IRenderBackend` for the window and frame, and an `IRenderer` for drawing.

1. **The renderer.** Implement `IRenderer`. Most of the work is backend
   neutral and already done: a `FramePlan` (`framePlan.h`) takes the frame's
   draws (`begin`, `addMesh`, `addGrass`, `addUi`, then `finish`) and works out
   which instances draw in which pass (lit, see-through back to front, the
   sun's shadow, each shadowed point light's), the point lights and the
   clusters they light, and every pass's uniforms. Your renderer uploads what
   it worked out and records the passes. Report what you can't do through
   `capabilities()`, and the engine leaves it out.
2. **The shaders.** The engine's shaders are WGSL (`engine/src/shaders/`), and
   material graphs compile to WGSL. A backend on an API that doesn't take WGSL
   translates them.
3. **The backend.** Implement `IRenderBackend`: create the window in `init()`,
   build a `SceneServices` (the asset store and your renderer) that
   `sceneServices()` returns, and point your renderer at each frame's target in
   `beginFrame()`. `Game::setup()` calls `sceneServices()` straight after the
   backend initialises.
4. **Offer it.** `RenderBackends::add("mine", [] { return
   std::make_unique<MyBackend>(); })`, before the game starts, or return it
   from `makeRenderBackend()`.

`WebGpuRenderBackend` and `WebGpuRenderer` are the reference implementation.
