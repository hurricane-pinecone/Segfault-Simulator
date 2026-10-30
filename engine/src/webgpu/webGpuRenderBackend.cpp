#include "engine/webgpu/webGpuRenderBackend.h"

#include "engine/runtime/rendering/backend/renderBackends.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/assetStore/textureImporter.h"

#include "engine/runtime/assetStore/assetStore.h"
#include <algorithm>
#include <cstdlib>

#include "SDL.h"

// No ImGui on the web: a web build is a game, not a tool.
#ifndef ENGINE_WEB
  #include "imgui.h"
  #include "imgui/backends/imgui_impl_sdl2.h"
  #include "imgui/backends/imgui_impl_wgpu.h"
  #include "webgpu/imgui/imguiViewportRenderer.h"
#endif

namespace sfs
{

WebGpuRenderBackend::WebGpuRenderBackend() = default;
WebGpuRenderBackend::~WebGpuRenderBackend() { shutdown(); }

bool WebGpuRenderBackend::init(const char* title, int width, int height)
{
  m_width = width;
  m_height = height;

  m_window = SDL_CreateWindow(
      title,
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      width,
      height,
      // Hidden until the first frame is drawn, so it
      // doesn't sit black while the GPU sets up.
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
  if (!m_window)
  {
    LOG_ERROR("Error creating window");
    return false;
  }

  if (!m_context.init(m_window))
    return false;
  m_context.configure(width, height);

  m_renderer = std::make_unique<WebGpuRenderer>(m_context);
  if (!m_renderer->initialize())
    return false;

  if (const char* path = std::getenv("SFS_SCREENSHOT"))
  {
    m_screenshotPath = path;
    if (const char* frame = std::getenv("SFS_SCREENSHOT_FRAME"))
      m_screenshotFrame = std::max(std::atoi(frame), 1);
    m_screenshotTarget = std::make_unique<WebGpuRenderTarget>(m_context);
  }

  m_assetStore = std::make_unique<AssetStore>();
  // Plain images any game can tint: a white pixel, and a white dot fading out
  // to its edge (soft particles and marks).
  m_assetStore->addWhitePixelTexture("white_pixel");
  m_assetStore->addRadialTexture("white_dot", 32);
  m_services.emplace(
      SceneServices{.assetStore = *m_assetStore, .renderer = m_renderer.get()});

  return initImGui();
}

void WebGpuRenderBackend::clearWindow()
{
  WGPURenderPassColorAttachment color = {};
  color.view = m_view;
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};

  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;

  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

bool WebGpuRenderBackend::initImGui()
{
#ifdef ENGINE_WEB
  return true;
#else
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard |
                    ImGuiConfigFlags_DockingEnable |
                    ImGuiConfigFlags_ViewportsEnable;

  // A detached panel is a real OS window: square it off and make it opaque
  // so it looks like one rather than a floating overlay.
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0.0f;
  style.Colors[ImGuiCol_WindowBg].w = 1.0f;

  if (!ImGui_ImplSDL2_InitForOther(m_window))
  {
    LOG_ERROR("ImGui: SDL2 platform backend failed to initialise");
    return false;
  }

  ImGui_ImplWGPU_InitInfo info;
  info.Device = m_context.device();
  info.RenderTargetFormat = m_context.format();
  if (!ImGui_ImplWGPU_Init(&info))
  {
    LOG_ERROR("ImGui: WebGPU renderer backend failed to initialise");
    return false;
  }

  imguiViewports::install(m_context);
  m_imguiReady = true;
  return true;
#endif
}

void WebGpuRenderBackend::shutdownImGui()
{
#ifndef ENGINE_WEB
  if (!m_imguiReady)
    return;

  imguiViewports::shutdown();
  ImGui_ImplWGPU_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  m_imguiReady = false;
#endif
}

void WebGpuRenderBackend::imguiProcessEvent(const SDL_Event& event)
{
#ifdef ENGINE_WEB
  (void)event;
#else
  if (m_imguiReady)
    ImGui_ImplSDL2_ProcessEvent(&event);
#endif
}

void WebGpuRenderBackend::imguiNewFrame()
{
#ifndef ENGINE_WEB
  if (!m_imguiReady)
    return;

  ImGui_ImplWGPU_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
#endif
}

void WebGpuRenderBackend::imguiRenderDrawData()
{
#ifndef ENGINE_WEB
  if (!m_imguiReady)
    return;

  ImGui::Render();
  m_imguiRendered = true;

  // The main window's panels draw over the scene in this frame's encoder.
  if (!m_encoder || !m_view)
    return;

  WGPURenderPassColorAttachment color = {};
  color.view = m_view;
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = WGPULoadOp_Load;
  color.storeOp = WGPUStoreOp_Store;

  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;

  WGPURenderPassEncoder pass =
      wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
#endif
}

SceneServices* WebGpuRenderBackend::sceneServices()
{
  return m_services ? &*m_services : nullptr;
}

void WebGpuRenderBackend::onResize(int width, int height)
{
  m_width = width;
  m_height = height;
  m_context.configure(width, height);
}

void WebGpuRenderBackend::beginFrame(int width, int height)
{
  m_width = width;
  m_height = height;

  wgpuSurfaceGetCurrentTexture(m_context.surface(), &m_surfTex);
  if (m_surfTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      m_surfTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
  {
    // Lost/outdated surface: reconfigure and skip recording this frame.
    m_context.configure(width, height);
    m_view = nullptr;
    m_encoder = nullptr;
    m_renderer->setTarget(nullptr, nullptr, {width, height});
    return;
  }

  m_view = m_context.surfaceView(m_surfTex.texture);
  m_encoder = wgpuDeviceCreateCommandEncoder(m_context.device(), nullptr);

  // The screenshot's frame draws offscreen, to be read back; every other
  // frame still shows in the window.
  WebGpuRenderTarget* target = m_sceneTarget;
  if (m_screenshotTarget && m_frame + 1 == m_screenshotFrame)
  {
    m_screenshotTarget->resize({width, height});
    target = m_screenshotTarget.get();
  }

  if (target && target->valid())
  {
    // The scene draws offscreen; the window only shows ImGui, so clear it.
    m_renderer->setTarget(m_encoder, target->view(), target->size());
    clearWindow();
  }
  else
  {
    m_renderer->setTarget(m_encoder, m_view, {width, height});
  }
}

void WebGpuRenderBackend::endFrame()
{
  // The renderer recorded into this frame's encoder during the scene's
  // render; detach it before the encoder is released.
  m_renderer->setTarget(nullptr, nullptr, {m_width, m_height});

  if (m_encoder)
  {
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(m_encoder, nullptr);
    wgpuQueueSubmit(m_context.queue(), 1, &cmd);
#ifndef __EMSCRIPTEN__ // the browser shows the canvas itself
    wgpuSurfacePresent(m_context.surface());
#endif
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(m_encoder);
    m_encoder = nullptr;
  }
  if (m_screenshotTarget && ++m_frame == m_screenshotFrame)
    saveScreenshot();
  if (m_view)
  {
    wgpuTextureViewRelease(m_view);
    m_view = nullptr;
  }
  // Shown once the first frame is in (or, if a hidden window had nothing to
  // draw into, now anyway).
  if (!m_shown)
  {
    SDL_ShowWindow(m_window);
    m_shown = true;
  }
  if (m_surfTex.texture)
  {
    wgpuTextureRelease(m_surfTex.texture);
    m_surfTex.texture = nullptr;
  }

  // Detached ImGui panels render after the main frame is submitted: each
  // submits its own commands, which must not interleave with the main
  // frame's buffer writes.
#ifndef ENGINE_WEB
  if (m_imguiRendered)
  {
    imguiViewports::renderPlatformWindows();
    m_imguiRendered = false;
  }
#endif

  // Fire any pending async callbacks (e.g. timestamp readback).
  m_context.poll();
}

void WebGpuRenderBackend::shutdown()
{
  shutdownImGui(); // its GPU objects live on the device released below
  m_services.reset();
  m_assetStore.reset();
  m_renderer.reset(); // before the device it was built on
  m_context.release();
  if (m_window)
  {
    SDL_DestroyWindow(m_window);
    m_window = nullptr;
  }
}

namespace
{
// A picture uploaded once, sampled by ImGui.
class WebGpuImage final : public IImage
{
public:
  WebGpuImage(WebGpuContext& context,
              glm::ivec2 size,
              std::span<const uint8_t> rgba)
      : m_size(size)
  {
    WGPUTextureDescriptor desc = {};
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {uint32_t(size.x), uint32_t(size.y), 1};
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    m_texture.reset(wgpuDeviceCreateTexture(context.device(), &desc));

    WGPUTexelCopyTextureInfo destination = {};
    destination.texture = m_texture.get();
    destination.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = {};
    layout.bytesPerRow = uint32_t(size.x) * 4;
    layout.rowsPerImage = uint32_t(size.y);
    wgpuQueueWriteTexture(context.queue(),
                          &destination,
                          rgba.data(),
                          rgba.size(),
                          &layout,
                          &desc.size);
    m_view.reset(wgpuTextureCreateView(m_texture.get(), nullptr));
  }

  glm::ivec2 size() const override { return m_size; }
  uint64_t imguiTexture() const override
  {
    return reinterpret_cast<uintptr_t>(m_view.get());
  }

private:
  glm::ivec2 m_size;
  wgpu::Texture m_texture;
  wgpu::TextureView m_view;
};
} // namespace

std::unique_ptr<IRenderTarget> WebGpuRenderBackend::createRenderTarget()
{
  return std::make_unique<WebGpuRenderTarget>(m_context);
}

void WebGpuRenderBackend::setSceneTarget(IRenderTarget* target)
{
  m_sceneTarget = static_cast<WebGpuRenderTarget*>(target);
}

std::unique_ptr<IRenderer>
WebGpuRenderBackend::createRenderer(uint32_t shadowMapResolution)
{
  auto renderer =
      std::make_unique<WebGpuRenderer>(m_context, shadowMapResolution);
  if (!renderer->initialize())
    return nullptr;
  return renderer;
}

void WebGpuRenderBackend::drawInto(IRenderTarget& target,
                                   IRenderer& renderer,
                                   const std::function<void()>& draw)
{
  auto& into = static_cast<WebGpuRenderTarget&>(target);
  auto& gpu = static_cast<WebGpuRenderer&>(renderer);
  if (!into.valid())
    return;
  const WGPUCommandEncoder encoder =
      wgpuDeviceCreateCommandEncoder(m_context.device(), nullptr);
  gpu.setTarget(encoder, into.view(), into.size());
  draw();
  gpu.setTarget(nullptr, nullptr, into.size());
  const WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(m_context.queue(), 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);
}

std::unique_ptr<IImage>
WebGpuRenderBackend::createImage(glm::ivec2 size, std::span<const uint8_t> rgba)
{
  if (size.x <= 0 || size.y <= 0 ||
      rgba.size() < std::size_t(size.x) * size.y * 4)
    return nullptr;
  return std::make_unique<WebGpuImage>(m_context, size, rgba);
}

void registerWebGpuBackend()
{
  RenderBackends::add(
      "webgpu", [] { return std::make_unique<WebGpuRenderBackend>(); });
}

void WebGpuRenderBackend::saveScreenshot()
{
  const glm::ivec2 size = m_screenshotTarget->size();
  if (size.x <= 0 || size.y <= 0)
    return;
  // Rows padded to 256 bytes, as copies need.
  const uint32_t rowBytes = (uint32_t(size.x) * 4 + 255) / 256 * 256;
  WGPUBufferDescriptor bufferDesc = {};
  bufferDesc.size = uint64_t(rowBytes) * size.y;
  bufferDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  const wgpu::Buffer readback(
      wgpuDeviceCreateBuffer(m_context.device(), &bufferDesc));
  const WGPUCommandEncoder encoder =
      wgpuDeviceCreateCommandEncoder(m_context.device(), nullptr);
  WGPUTexelCopyTextureInfo source = {};
  source.texture = m_screenshotTarget->texture();
  source.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo destination = {};
  destination.buffer = readback.get();
  destination.layout.bytesPerRow = rowBytes;
  destination.layout.rowsPerImage = uint32_t(size.y);
  const WGPUExtent3D extent = {uint32_t(size.x), uint32_t(size.y), 1};
  wgpuCommandEncoderCopyTextureToBuffer(
      encoder, &source, &destination, &extent);
  const WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(m_context.queue(), 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);

  bool mapped = false;
  WGPUBufferMapCallbackInfo callback = {};
  callback.mode = WGPUCallbackMode_AllowProcessEvents;
  callback.callback =
      [](WGPUMapAsyncStatus status, WGPUStringView, void* done, void*)
  { *static_cast<bool*>(done) = status == WGPUMapAsyncStatus_Success; };
  callback.userdata1 = &mapped;
  wgpuBufferMapAsync(
      readback.get(), WGPUMapMode_Read, 0, bufferDesc.size, callback);
  for (int i = 0; i < 100 && !mapped; ++i)
    m_context.poll(true);
  if (!mapped)
  {
    LOG_ERROR("Couldn't read the screenshot back");
    return;
  }
  const auto* data = static_cast<const uint8_t*>(
      wgpuBufferGetConstMappedRange(readback.get(), 0, bufferDesc.size));
  TextureData image;
  image.width = uint32_t(size.x);
  image.height = uint32_t(size.y);
  image.rgba.resize(std::size_t(size.x) * size.y * 4);
  const WGPUTextureFormat format = m_context.format();
  const bool bgra = format == WGPUTextureFormat_BGRA8Unorm ||
                    format == WGPUTextureFormat_BGRA8UnormSrgb;
  for (int y = 0; y < size.y; ++y)
    for (int x = 0; x < size.x; ++x)
    {
      const uint8_t* from = data + std::size_t(y) * rowBytes + x * 4;
      uint8_t* to = &image.rgba[(std::size_t(y) * size.x + x) * 4];
      to[0] = bgra ? from[2] : from[0];
      to[1] = from[1];
      to[2] = bgra ? from[0] : from[2];
      to[3] = 255;
    }
  wgpuBufferUnmap(readback.get());
  if (saveTexturePng(image, m_screenshotPath))
    LOG_INFO("Screenshot saved to " + m_screenshotPath);
}

} // namespace sfs
