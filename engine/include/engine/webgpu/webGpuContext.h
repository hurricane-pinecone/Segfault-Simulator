#pragma once

#include <webgpu/webgpu.h>

struct SDL_Window;

namespace sfs
{

struct WebGpuDeviceState;

// Owns the WebGPU instance, surface, adapter, device and queue for a window,
// and the surface configuration (format + present mode). The full adapter
// limits are requested so multi-hundred-MB storage buffers are allowed.
class WebGpuContext
{
public:
  bool init(SDL_Window* window);

  /**
   * A device with no window or surface, drawing into `format` textures the
   * caller makes (offscreen rendering, tests). False (logged) if there's no
   * usable adapter, e.g. a machine with no GPU driver.
   */
  bool initHeadless(WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm);

  // Configure the window's surface at this size, in pixels (nothing for a
  // size of 0: a minimized window keeps its last).
  void configure(int width, int height);

  // Wait for the display when presenting (vsync), capping the frame rate at
  // its refresh rate (a shipped game, an editor). Off by default, so frame
  // timings show what the GPU can do. Before init(), or after: then the
  // surface is configured again at once.
  void setVsync(bool vsync);

  // Configure another window's surface (e.g. an ImGui panel dragged out into
  // its own OS window) like the main one: same device and format. With vsync
  // it doesn't wait for the display too: the main window already paces the
  // frame, and waiting once per window would slow it down.
  void configureSurface(WGPUSurface surface, int width, int height) const;

  // A view to draw into a surface's texture this frame, in format(): sRGB,
  // even where the surface itself isn't (a browser canvas), so shaders' linear
  // colour shows the same everywhere.
  WGPUTextureView surfaceView(WGPUTexture texture) const;
  void release();

  WGPUInstance instance() const { return m_instance; }
  WGPUSurface surface() const { return m_surface; }
  WGPUAdapter adapter() const { return m_adapter; }
  // Whether wgpu runs on OpenGL (its fallback for machines without Vulkan,
  // D3D12 or Metal), which can't do everything the others can.
  bool onOpenGL() const { return m_onOpenGL; }
  WGPUDevice device() const { return m_device; }
  WGPUQueue queue() const { return m_queue; }
  // The format everything drawn to a window renders in (sRGB where the
  // surface offers it or can be viewed as it).
  WGPUTextureFormat format() const { return m_format; }

  // Run the device's finished async work's callbacks (buffer maps, timestamp
  // readbacks); with `wait`, first wait a little for some to finish.
  void poll(bool wait = false) const;

  // Whether the device was created with GPU timestamp queries (benchmark HUD).
  bool hasTimestamps() const { return m_hasTimestamps; }

  // Whether the device has been lost (a driver crash or update, the GPU
  // removed, out of memory): nothing more can be drawn with it.
  bool deviceLost() const;

  // The widest or tallest texture the device can make, in texels.
  uint32_t maxTextureSize() const { return m_maxTextureSize; }

private:
  // What the device can do (maxTextureSize()).
  void readLimits();

  WGPUInstance m_instance = nullptr;
  WGPUSurface m_surface = nullptr;
  WGPUAdapter m_adapter = nullptr;
  bool m_onOpenGL = false;
  WGPUDevice m_device = nullptr;
  WGPUQueue m_queue = nullptr;
  WGPUTextureFormat m_format = WGPUTextureFormat_BGRA8Unorm;
  WGPUTextureFormat m_surfaceFormat = WGPUTextureFormat_BGRA8Unorm; // its own
  WGPUPresentMode m_present = WGPUPresentMode_Fifo;  // the main window's
  WGPUPresentMode m_unsynced = WGPUPresentMode_Fifo; // the fastest available
  bool m_vsync = false;
  bool m_hasTimestamps = false;
  int m_width = 0; // the main surface's configured size (0: not yet)
  int m_height = 0;
  uint32_t m_maxTextureSize = 8192;

  // Told by the device-lost callback, which may come at any time (even after
  // this context is gone), so it outlives the context: one small block a
  // device, kept for the program's life.
  WebGpuDeviceState* m_deviceState = nullptr;
};

} // namespace sfs
