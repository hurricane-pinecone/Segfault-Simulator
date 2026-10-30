#pragma once

#include <webgpu/webgpu.h>

struct SDL_Window;

namespace sfs
{

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

  void configure(int width, int height);

  // Wait for the display when presenting (vsync), capping the frame rate at
  // its refresh rate (e.g. an editor, which needn't redraw faster). Off by
  // default, so frame timings show what the GPU can do. Set before init().
  void setVsync(bool vsync) { m_vsync = vsync; }

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

private:
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
};

} // namespace sfs
