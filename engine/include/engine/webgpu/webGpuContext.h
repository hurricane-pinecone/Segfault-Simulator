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
  void configure(int width, int height);
  void release();

  WGPUInstance instance() const { return m_instance; }
  WGPUSurface surface() const { return m_surface; }
  WGPUAdapter adapter() const { return m_adapter; }
  WGPUDevice device() const { return m_device; }
  WGPUQueue queue() const { return m_queue; }
  WGPUTextureFormat format() const { return m_format; }

  // Whether the device was created with GPU timestamp queries (benchmark HUD).
  bool hasTimestamps() const { return m_hasTimestamps; }

private:
  WGPUInstance m_instance = nullptr;
  WGPUSurface m_surface = nullptr;
  WGPUAdapter m_adapter = nullptr;
  WGPUDevice m_device = nullptr;
  WGPUQueue m_queue = nullptr;
  WGPUTextureFormat m_format = WGPUTextureFormat_BGRA8Unorm;
  WGPUPresentMode m_present = WGPUPresentMode_Fifo;
  bool m_hasTimestamps = false;
};

} // namespace sfs
