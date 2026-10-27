#include "engine/webgpu/webGpuRenderBackend.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/assetStore/assetStore.h"

#include "SDL.h"
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

  return true;
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
  m_renderer3D->setTarget(m_encoder, m_view, {width, height});
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

  // Fire any pending async callbacks (e.g. timestamp readback).
  wgpuDevicePoll(m_context.device(), 0, nullptr);
}

void WebGpuRenderBackend::shutdown()
{
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
