#include "engine/webgpu/webGpuContext.h"

#include "engine/core/logger/logger.h"

#include <sdl2webgpu.h>

#include <cstddef>
#include <string>

namespace sfs
{
namespace
{
void logStr(const char* label, WGPUStringView s)
{
  LOG_ERROR(std::string(label) + ": " +
            std::string(s.data ? s.data : "", s.data ? s.length : 0));
}

WGPUAdapter requestAdapterSync(WGPUInstance instance, WGPUSurface surface)
{
  struct Out
  {
    WGPUAdapter adapter = nullptr;
  } out;
  WGPURequestAdapterOptions opts = {};
  opts.compatibleSurface = surface;
  opts.powerPreference = WGPUPowerPreference_HighPerformance;
  WGPURequestAdapterCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowProcessEvents;
  cb.callback = [](WGPURequestAdapterStatus status,
                   WGPUAdapter adapter,
                   WGPUStringView message,
                   void* ud1,
                   void*)
  {
    if (status == WGPURequestAdapterStatus_Success)
      static_cast<Out*>(ud1)->adapter = adapter;
    else
      logStr("requestAdapter failed", message);
  };
  cb.userdata1 = &out;
  wgpuInstanceRequestAdapter(instance, &opts, cb);
  return out.adapter;
}

WGPUDevice requestDeviceSync(WGPUAdapter adapter, bool* hasTimestamps)
{
  struct Out
  {
    WGPUDevice device = nullptr;
  } out;
  // Request the adapter's full limits so big buffers (past the 128MB storage
  // binding default) are allowed.
  WGPULimits limits = {};
  wgpuAdapterGetLimits(adapter, &limits);
  // An adapter with no buffer-size cap (e.g. Vulkan) reports UINT64_MAX, which
  // is also WGPU_LIMIT_U64_UNDEFINED: passed back as-is it reads as "unset" and
  // the device silently gets the 256MB default.
  if (limits.maxBufferSize == WGPU_LIMIT_U64_UNDEFINED)
    limits.maxBufferSize = WGPU_LIMIT_U64_UNDEFINED - 1;
  WGPUAdapterInfo info = {};
  wgpuAdapterGetInfo(adapter, &info);
  LOG_INFO("WebGPU adapter: " +
           std::string(info.device.data ? info.device.data : "",
                       info.device.data ? info.device.length : 0) +
           ", maxStorageBufferBindingSize " +
           std::to_string(limits.maxStorageBufferBindingSize));
  wgpuAdapterInfoFreeMembers(info);
  WGPUDeviceDescriptor desc = {};
  desc.requiredLimits = &limits;
  // Enable GPU timestamp queries when the adapter supports them (benchmark
  // HUD).
  WGPUFeatureName feats[1] = {WGPUFeatureName_TimestampQuery};
  *hasTimestamps =
      wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery);
  if (*hasTimestamps)
  {
    desc.requiredFeatureCount = 1;
    desc.requiredFeatures = feats;
  }
  desc.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*,
                                                 WGPUErrorType type,
                                                 WGPUStringView message,
                                                 void*,
                                                 void*)
  {
    LOG_ERROR("WebGPU error (" + std::to_string(static_cast<int>(type)) +
              "): " +
              std::string(message.data ? message.data : "",
                          message.data ? message.length : 0));
  };
  WGPURequestDeviceCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowProcessEvents;
  cb.callback = [](WGPURequestDeviceStatus status,
                   WGPUDevice device,
                   WGPUStringView message,
                   void* ud1,
                   void*)
  {
    if (status == WGPURequestDeviceStatus_Success)
      static_cast<Out*>(ud1)->device = device;
    else
      logStr("requestDevice failed", message);
  };
  cb.userdata1 = &out;
  wgpuAdapterRequestDevice(adapter, &desc, cb);
  return out.device;
}
} // namespace

bool WebGpuContext::init(SDL_Window* window)
{
  WGPUInstanceDescriptor instDesc = {};
  m_instance = wgpuCreateInstance(&instDesc);
  if (!m_instance)
  {
    LOG_ERROR("Failed to create WebGPU instance");
    return false;
  }

  m_surface = SDL_GetWGPUSurface(m_instance, window);
  m_adapter = requestAdapterSync(m_instance, m_surface);
  if (!m_adapter)
  {
    LOG_ERROR("Failed to acquire WebGPU adapter");
    return false;
  }

  m_device = requestDeviceSync(m_adapter, &m_hasTimestamps);
  if (!m_device)
  {
    LOG_ERROR("Failed to acquire WebGPU device");
    return false;
  }
  m_queue = wgpuDeviceGetQueue(m_device);

  WGPUSurfaceCapabilities caps = {};
  wgpuSurfaceGetCapabilities(m_surface, m_adapter, &caps);
  m_format =
      caps.formatCount > 0 ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;

  // Unless vsync is asked for, prefer an unsynced present mode so frame
  // timing reflects real GPU throughput (Mailbox = no tearing; else
  // Immediate); fall back to vsync (Fifo, always available).
  m_unsynced = WGPUPresentMode_Fifo;
  for (size_t i = 0; i < caps.presentModeCount; ++i)
  {
    if (caps.presentModes[i] == WGPUPresentMode_Mailbox)
    {
      m_unsynced = WGPUPresentMode_Mailbox;
      break;
    }
    if (caps.presentModes[i] == WGPUPresentMode_Immediate)
      m_unsynced = WGPUPresentMode_Immediate;
  }
  m_present = m_vsync ? WGPUPresentMode_Fifo : m_unsynced;

  return true;
}

bool WebGpuContext::initHeadless(WGPUTextureFormat format)
{
  WGPUInstanceDescriptor instDesc = {};
  m_instance = wgpuCreateInstance(&instDesc);
  if (!m_instance)
  {
    LOG_ERROR("Failed to create WebGPU instance");
    return false;
  }
  m_adapter = requestAdapterSync(m_instance, nullptr);
  if (!m_adapter)
  {
    LOG_ERROR("Failed to acquire WebGPU adapter");
    return false;
  }
  m_device = requestDeviceSync(m_adapter, &m_hasTimestamps);
  if (!m_device)
  {
    LOG_ERROR("Failed to acquire WebGPU device");
    return false;
  }
  m_queue = wgpuDeviceGetQueue(m_device);
  m_format = format;
  return true;
}

void WebGpuContext::configure(int width, int height)
{
  configureSurface(m_surface, width, height);
}

void WebGpuContext::configureSurface(WGPUSurface surface,
                                     int width,
                                     int height) const
{
  WGPUSurfaceConfiguration cfg = {};
  cfg.device = m_device;
  cfg.format = m_format;
  cfg.usage = WGPUTextureUsage_RenderAttachment;
  cfg.width = static_cast<uint32_t>(width);
  cfg.height = static_cast<uint32_t>(height);
  cfg.presentMode = surface == m_surface ? m_present : m_unsynced;
  cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
  wgpuSurfaceConfigure(surface, &cfg);
}

void WebGpuContext::release()
{
  if (m_surface)
    wgpuSurfaceUnconfigure(m_surface);
  // wgpu-native cleans device/adapter/instance/surface up at process exit; the
  // app holds a single context for its whole lifetime.
  m_queue = nullptr;
  m_device = nullptr;
  m_adapter = nullptr;
  m_surface = nullptr;
  m_instance = nullptr;
}

} // namespace sfs
