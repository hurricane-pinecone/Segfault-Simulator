#include "engine/webgpu/webGpuRenderBackend.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "webgpu/imgui/imguiViewportRenderer.h"

#include "SDL.h"
#include "imgui.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_wgpu.h"
#include <webgpu/wgpu.h> // wgpuDevicePoll

namespace sfs
{

WebGpuRenderBackend::WebGpuRenderBackend() = default;
WebGpuRenderBackend::~WebGpuRenderBackend() { shutdown(); }

bool WebGpuRenderBackend::init(const char* title, int width, int height)
{
  m_width = width;
  m_height = height;

  m_window = SDL_CreateWindow(title,
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              width,
                              height,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!m_window)
  {
    LOG_ERROR("Error creating window");
    return false;
  }

  if (!m_context.init(m_window))
    return false;
  m_context.configure(width, height);

  m_renderer3D = std::make_unique<WebGpuRenderer3D>(m_context);
  if (!m_renderer3D->initialize())
    return false;

  m_assetStore = std::make_unique<AssetStore>();
  m_services.emplace(SceneServices{.assetStore = *m_assetStore,
                                   .renderer3D = m_renderer3D.get()});

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

  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

bool WebGpuRenderBackend::initImGui()
{
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
}

void WebGpuRenderBackend::shutdownImGui()
{
  if (!m_imguiReady)
    return;

  imguiViewports::shutdown();
  ImGui_ImplWGPU_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  m_imguiReady = false;
}

void WebGpuRenderBackend::imguiProcessEvent(const SDL_Event& event)
{
  if (m_imguiReady)
    ImGui_ImplSDL2_ProcessEvent(&event);
}

void WebGpuRenderBackend::imguiNewFrame()
{
  if (!m_imguiReady)
    return;

  ImGui_ImplWGPU_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
}

void WebGpuRenderBackend::imguiRenderDrawData()
{
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

  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(m_encoder, &desc);
  ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
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
    m_renderer3D->setTarget(nullptr, nullptr, {width, height});
    return;
  }

  m_view = wgpuTextureCreateView(m_surfTex.texture, nullptr);
  m_encoder = wgpuDeviceCreateCommandEncoder(m_context.device(), nullptr);

  if (m_sceneTarget && m_sceneTarget->valid())
  {
    // The scene draws offscreen; the window only shows ImGui, so clear it.
    m_renderer3D->setTarget(m_encoder, m_sceneTarget->view(), m_sceneTarget->size());
    clearWindow();
  }
  else
  {
    m_renderer3D->setTarget(m_encoder, m_view, {width, height});
  }
}

void WebGpuRenderBackend::endFrame()
{
  // The 3D renderer recorded into this frame's encoder during the scene's
  // render; detach it before the encoder is released.
  m_renderer3D->setTarget(nullptr, nullptr, {m_width, m_height});

  if (m_encoder)
  {
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(m_encoder, nullptr);
    wgpuQueueSubmit(m_context.queue(), 1, &cmd);
    wgpuSurfacePresent(m_context.surface());
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(m_encoder);
    m_encoder = nullptr;
  }
  if (m_view)
  {
    wgpuTextureViewRelease(m_view);
    m_view = nullptr;
  }
  if (m_surfTex.texture)
  {
    wgpuTextureRelease(m_surfTex.texture);
    m_surfTex.texture = nullptr;
  }

  // Detached ImGui panels render after the main frame is submitted: each
  // submits its own commands, which must not interleave with the main
  // frame's buffer writes.
  if (m_imguiRendered)
  {
    imguiViewports::renderPlatformWindows();
    m_imguiRendered = false;
  }

  // Fire any pending async callbacks (e.g. timestamp readback).
  wgpuDevicePoll(m_context.device(), 0, nullptr);
}

void WebGpuRenderBackend::shutdown()
{
  shutdownImGui(); // its GPU objects live on the device released below
  m_services.reset();
  m_assetStore.reset();
  m_renderer3D.reset(); // before the device it was built on
  m_context.release();
  if (m_window)
  {
    SDL_DestroyWindow(m_window);
    m_window = nullptr;
  }
}

} // namespace sfs
