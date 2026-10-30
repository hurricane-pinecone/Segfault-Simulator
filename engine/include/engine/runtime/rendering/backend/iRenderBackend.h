#pragma once

#include "engine/runtime/rendering/backend/renderTarget.h"
#include "engine/runtime/rendering/iRenderer.h"

#include <SDL_events.h>
#include <SDL_video.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace sfs
{

struct SceneServices;

// The platform window + GPU context + per-frame lifecycle a Game draws through.
// One concrete backend per graphics API (WebGPU today). Game owns one and
// drives it; scenes never touch it directly -- they reach drawing through the
// SceneServices the backend supplies.
class IRenderBackend
{
public:
  virtual ~IRenderBackend() = default;

  // Its name among RenderBackends ("webgpu", "opengl", ...), for logs and
  // settings.
  virtual std::string_view name() const { return {}; }

  // Create the window + graphics context/device. Returns false (and logs) on
  // failure. SDL is already initialised by Game before this runs.
  virtual bool init(const char* title, int width, int height) = 0;

  // The SDL window, for the event/input routing Game owns.
  virtual SDL_Window* window() const = 0;

  virtual void onResize(int width, int height) = 0;

  // The drawing/asset services this backend offers scenes. Owned by the backend
  // and valid until shutdown(); built during init(). A compute-only backend
  // returns a bundle whose services are present but unused.
  virtual SceneServices* sceneServices() = 0;

  // Per-frame lifecycle: beginFrame acquires the backbuffer and clears it;
  // endFrame presents.
  virtual void beginFrame(int width, int height) = 0;
  virtual void endFrame() = 0;

  // ImGui platform/renderer integration. No-ops on a backend without one;
  // imguiAvailable() gates the debug overlay in Game.
  virtual void imguiProcessEvent(const SDL_Event& event) {}
  virtual void imguiNewFrame() {}
  virtual void imguiRenderDrawData() {}
  virtual bool imguiAvailable() const { return false; }

  // Wait for the display when presenting (vsync): no tearing, and no faster
  // than the screen shows. Before init() or any time after (it takes effect
  // from the next frame).
  virtual void setVsync(bool) {}

  // Whether the GPU device was lost (a driver crash or update, the GPU
  // removed): nothing more can be drawn. Game checks after each frame.
  virtual bool deviceLost() const { return false; }

  // --- For tools (the editor). A backend without them returns null, and the
  // tool shows less (no viewport panel, previews or thumbnails).

  // An offscreen picture to draw into and show in a panel.
  virtual std::unique_ptr<IRenderTarget> createRenderTarget()
  {
    return nullptr;
  }

  // Draw the scene into `target` (one of createRenderTarget()'s) instead of
  // the window, which then only shows ImGui; null: back to the window.
  virtual void setSceneTarget(IRenderTarget* target) {}

  // Another renderer on this backend's device, for previews drawn apart from
  // the scene (shadow maps of `shadowMapResolution` pixels).
  virtual std::unique_ptr<IRenderer>
  createRenderer(uint32_t shadowMapResolution)
  {
    return nullptr;
  }

  // Draw one frame of `renderer` (one of createRenderer()'s) into `target`:
  // `draw` runs its beginFrame(), draws and endFrame().
  virtual void drawInto(IRenderTarget& target,
                        IRenderer& renderer,
                        const std::function<void()>& draw)
  {
  }

  // A picture of `size` pixels from tightly packed 8-bit RGBA, top row first.
  virtual std::unique_ptr<IImage> createImage(glm::ivec2 size,
                                              std::span<const uint8_t> rgba)
  {
    return nullptr;
  }

  virtual void shutdown() = 0;
};

} // namespace sfs
