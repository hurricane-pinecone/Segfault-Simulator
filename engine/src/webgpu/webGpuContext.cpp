#include "engine/webgpu/webGpuContext.h"

#include "engine/core/logger/logger.h"

#ifdef __EMSCRIPTEN__
  #include <emscripten/emscripten.h>
#else
  #include <sdl2webgpu.h>
  #include <webgpu/wgpu.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace sfs
{

struct WebGpuDeviceState
{
  std::atomic<bool> lost{false};
  std::atomic<bool> releasing{false}; // a loss now is the context's own doing
};

namespace
{
void logStr(const char* label, WGPUStringView s)
{
  LOG_ERROR(std::string(label) + ": " +
            std::string(s.data ? s.data : "", s.data ? s.length : 0));
}

// In a browser, requests answer once the page's event loop runs, so wait for
// them by yielding to it (the program is built with ASYNCIFY); natively, wgpu
// answers before the request returns.
#ifdef __EMSCRIPTEN__
constexpr WGPUCallbackMode kRequestMode = WGPUCallbackMode_AllowSpontaneous;
void waitFor(const bool& answered)
{
  while (!answered)
    emscripten_sleep(1);
}
#else
constexpr WGPUCallbackMode kRequestMode = WGPUCallbackMode_AllowProcessEvents;
void waitFor(const bool&) {}
#endif

#ifdef __EMSCRIPTEN__
// The browser's WebGPU: no backends to pick.
WGPUInstance createInstance() { return wgpuCreateInstance(nullptr); }
#else
// The instance, on the backends SFS_WGPU_BACKEND names (vulkan, gl, metal,
// dx12; default: whichever wgpu picks), e.g. to try the GL fallback.
WGPUInstance createInstance()
{
  WGPUInstanceExtras extras = {};
  extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
  extras.backends = WGPUInstanceBackend_All;
  if (const char* name = std::getenv("SFS_WGPU_BACKEND"))
  {
    const std::string backend(name);
    if (backend == "vulkan")
      extras.backends = WGPUInstanceBackend_Vulkan;
    else if (backend == "gl")
      extras.backends = WGPUInstanceBackend_GL;
    else if (backend == "metal")
      extras.backends = WGPUInstanceBackend_Metal;
    else if (backend == "dx12")
      extras.backends = WGPUInstanceBackend_DX12;
    LOG_INFO("WebGPU backends limited to " + backend);
  }
  WGPUInstanceDescriptor desc = {};
  desc.nextInChain = &extras.chain;
  return wgpuCreateInstance(&desc);
}
#endif

WGPUAdapter requestAdapterSync(WGPUInstance instance, WGPUSurface surface)
{
  struct Out
  {
    WGPUAdapter adapter = nullptr;
    bool answered = false;
  } out;
  WGPURequestAdapterOptions opts = {};
  opts.compatibleSurface = surface;
  opts.powerPreference = WGPUPowerPreference_HighPerformance;
  WGPURequestAdapterCallbackInfo cb = {};
  cb.mode = kRequestMode;
  cb.callback = [](WGPURequestAdapterStatus status,
                   WGPUAdapter adapter,
                   WGPUStringView message,
                   void* ud1,
                   void*)
  {
    Out& out = *static_cast<Out*>(ud1);
    if (status == WGPURequestAdapterStatus_Success)
      out.adapter = adapter;
    else
      logStr("requestAdapter failed", message);
    out.answered = true;
  };
  cb.userdata1 = &out;
  wgpuInstanceRequestAdapter(instance, &opts, cb);
  waitFor(out.answered);
  return out.adapter;
}

WGPUDevice requestDeviceSync(WGPUAdapter adapter,
                             bool* hasTimestamps,
                             WebGpuDeviceState* deviceState)
{
  struct Out
  {
    WGPUDevice device = nullptr;
    bool answered = false;
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
  // Losing the device (a driver crash or update, the GPU removed) ends the
  // game: Game asks deviceLost() after each frame.
  desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
  desc.deviceLostCallbackInfo.userdata1 = deviceState;
  desc.deviceLostCallbackInfo.callback = [](WGPUDevice const*,
                                            WGPUDeviceLostReason reason,
                                            WGPUStringView message,
                                            void* state,
                                            void*)
  {
    auto* device = static_cast<WebGpuDeviceState*>(state);
    // Released on purpose (shutting down): not a loss.
    if (reason == WGPUDeviceLostReason_Destroyed ||
        reason == WGPUDeviceLostReason_InstanceDropped ||
        (device && device->releasing.load()))
      return;
    logStr("The WebGPU device was lost", message);
    if (device)
      device->lost.store(true);
  };
  WGPURequestDeviceCallbackInfo cb = {};
  cb.mode = kRequestMode;
  cb.callback = [](WGPURequestDeviceStatus status,
                   WGPUDevice device,
                   WGPUStringView message,
                   void* ud1,
                   void*)
  {
    Out& out = *static_cast<Out*>(ud1);
    if (status == WGPURequestDeviceStatus_Success)
      out.device = device;
    else
      logStr("requestDevice failed", message);
    out.answered = true;
  };
  cb.userdata1 = &out;
  wgpuAdapterRequestDevice(adapter, &desc, cb);
  waitFor(out.answered);
  return out.device;
}

// The sRGB twin of a plain 8-bit colour format (the GPU then turns shaders'
// linear colour into screen colour); others as they are.
WGPUTextureFormat srgbOf(WGPUTextureFormat format)
{
  switch (format)
  {
  case WGPUTextureFormat_BGRA8Unorm:
    return WGPUTextureFormat_BGRA8UnormSrgb;
  case WGPUTextureFormat_RGBA8Unorm:
    return WGPUTextureFormat_RGBA8UnormSrgb;
  default:
    return format;
  }
}

// The window's surface: natively through SDL, in a browser the page's canvas
// (the one SDL draws to, "#canvas").
WGPUSurface createSurface(WGPUInstance instance, SDL_Window* window)
{
#ifdef __EMSCRIPTEN__
  (void)window;
  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {};
  canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
  canvas.selector = {"#canvas", WGPU_STRLEN};
  WGPUSurfaceDescriptor desc = {};
  desc.nextInChain = &canvas.chain;
  return wgpuInstanceCreateSurface(instance, &desc);
#else
  return SDL_GetWGPUSurface(instance, window);
#endif
}
} // namespace

bool WebGpuContext::init(SDL_Window* window)
{
  m_instance = createInstance();
  if (!m_instance)
  {
    LOG_ERROR("Failed to create WebGPU instance");
    return false;
  }

  m_surface = createSurface(m_instance, window);
  m_adapter = requestAdapterSync(m_instance, m_surface);
  if (m_adapter)
  {
    WGPUAdapterInfo info = {};
    wgpuAdapterGetInfo(m_adapter, &info);
    m_onOpenGL = info.backendType == WGPUBackendType_OpenGL ||
                 info.backendType == WGPUBackendType_OpenGLES;
    wgpuAdapterInfoFreeMembers(info);
  }
  if (!m_adapter)
  {
    LOG_ERROR("Failed to acquire WebGPU adapter");
    return false;
  }

  m_deviceState = new WebGpuDeviceState; // kept: see m_deviceState
  m_device = requestDeviceSync(m_adapter, &m_hasTimestamps, m_deviceState);
  if (!m_device)
  {
    LOG_ERROR("Failed to acquire WebGPU device");
    return false;
  }
  m_queue = wgpuDeviceGetQueue(m_device);
  readLimits();

  WGPUSurfaceCapabilities caps = {};
  wgpuSurfaceGetCapabilities(m_surface, m_adapter, &caps);
  m_surfaceFormat =
      caps.formatCount > 0 ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
  m_format = srgbOf(m_surfaceFormat);

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
  wgpuSurfaceCapabilitiesFreeMembers(caps);

  return true;
}

void WebGpuContext::readLimits()
{
  WGPULimits limits = {};
  if (wgpuDeviceGetLimits(m_device, &limits) == WGPUStatus_Success &&
      limits.maxTextureDimension2D > 0 &&
      limits.maxTextureDimension2D != WGPU_LIMIT_U32_UNDEFINED)
    m_maxTextureSize = limits.maxTextureDimension2D;
}

void WebGpuContext::setVsync(bool vsync)
{
  m_vsync = vsync;
  const WGPUPresentMode present = m_vsync ? WGPUPresentMode_Fifo : m_unsynced;
  if (present == m_present)
    return;
  m_present = present;
  if (m_surface && m_width > 0 && m_height > 0)
    configure(m_width, m_height);
}

bool WebGpuContext::deviceLost() const
{
  return m_deviceState && m_deviceState->lost.load();
}

bool WebGpuContext::initHeadless(WGPUTextureFormat format)
{
  m_instance = createInstance();
  if (!m_instance)
  {
    LOG_ERROR("Failed to create WebGPU instance");
    return false;
  }
  m_adapter = requestAdapterSync(m_instance, nullptr);
  if (m_adapter)
  {
    WGPUAdapterInfo info = {};
    wgpuAdapterGetInfo(m_adapter, &info);
    m_onOpenGL = info.backendType == WGPUBackendType_OpenGL ||
                 info.backendType == WGPUBackendType_OpenGLES;
    wgpuAdapterInfoFreeMembers(info);
  }
  if (!m_adapter)
  {
    LOG_ERROR("Failed to acquire WebGPU adapter");
    return false;
  }
  m_deviceState = new WebGpuDeviceState; // kept: see m_deviceState
  m_device = requestDeviceSync(m_adapter, &m_hasTimestamps, m_deviceState);
  if (!m_device)
  {
    LOG_ERROR("Failed to acquire WebGPU device");
    return false;
  }
  m_queue = wgpuDeviceGetQueue(m_device);
  readLimits();
  m_format = format;
  m_surfaceFormat = format;
  return true;
}

void WebGpuContext::configure(int width, int height)
{
  if (!m_surface || width <= 0 || height <= 0)
    return;
  m_width = width;
  m_height = height;
  configureSurface(m_surface, width, height);
}

void WebGpuContext::configureSurface(WGPUSurface surface,
                                     int width,
                                     int height) const
{
  // A surface of no size can't be configured (a minimized window's).
  if (!surface || !m_device || width <= 0 || height <= 0)
    return;
  WGPUSurfaceConfiguration cfg = {};
  cfg.device = m_device;
  cfg.format = m_surfaceFormat;
  cfg.usage = WGPUTextureUsage_RenderAttachment;
  // Drawn through sRGB views of it where it isn't sRGB itself.
  if (m_format != m_surfaceFormat)
  {
    cfg.viewFormatCount = 1;
    cfg.viewFormats = &m_format;
  }
  cfg.width = static_cast<uint32_t>(width);
  cfg.height = static_cast<uint32_t>(height);
  cfg.presentMode = surface == m_surface ? m_present : m_unsynced;
  cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
  wgpuSurfaceConfigure(surface, &cfg);
}

WGPUTextureView WebGpuContext::surfaceView(WGPUTexture texture) const
{
  WGPUTextureViewDescriptor desc = {};
  desc.format = m_format;
  desc.dimension = WGPUTextureViewDimension_2D;
  desc.mipLevelCount = 1;
  desc.arrayLayerCount = 1;
  desc.aspect = WGPUTextureAspect_All;
  return wgpuTextureCreateView(texture, &desc);
}

void WebGpuContext::poll(bool wait) const
{
#ifdef __EMSCRIPTEN__
  // The browser finishes work between frames; let it run, then take the
  // answers.
  if (wait)
    emscripten_sleep(1);
  wgpuInstanceProcessEvents(m_instance);
#else
  wgpuDevicePoll(m_device, wait, nullptr);
#endif
}

void WebGpuContext::release()
{
  if (m_deviceState)
    m_deviceState->releasing.store(true);
  if (m_surface && m_width > 0)
    wgpuSurfaceUnconfigure(m_surface);
  // Device last but one (what was made on it holds references to it), then
  // what it came from.
  if (m_queue)
    wgpuQueueRelease(m_queue);
  if (m_device)
    wgpuDeviceRelease(m_device);
  if (m_adapter)
    wgpuAdapterRelease(m_adapter);
  if (m_surface)
    wgpuSurfaceRelease(m_surface);
  if (m_instance)
    wgpuInstanceRelease(m_instance);
  m_queue = nullptr;
  m_device = nullptr;
  m_adapter = nullptr;
  m_surface = nullptr;
  m_instance = nullptr;
  m_width = m_height = 0;
  m_deviceState = nullptr; // kept for a late callback; see the header
}

} // namespace sfs
