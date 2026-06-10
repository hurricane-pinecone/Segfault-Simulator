#pragma once

#include <SDL_events.h>
#include <SDL_video.h>

namespace sfs
{

struct SceneServices;

// The platform window + GPU context + per-frame lifecycle a Game draws through.
// One concrete backend per graphics API (OpenGL, WebGPU). Game owns one and
// drives it; scenes never touch it directly -- they reach drawing through the
// SceneServices the backend supplies.
class IRenderBackend
{
public:
  virtual ~IRenderBackend() = default;

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

  virtual void shutdown() = 0;
};

} // namespace sfs
