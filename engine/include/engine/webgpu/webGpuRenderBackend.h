#pragma once

#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/webgpu/render3d/webGpuRenderer3D.h"
#include "engine/webgpu/webGpuRenderTarget.h"
#include "engine/webgpu/webGpuContext.h"

#include <webgpu/webgpu.h>

#include <memory>
#include <optional>

namespace sfs
{

class AssetStore;

// WebGPU render backend: an SDL window (no GL context) plus a WebGPU device and
// surface. Each frame it opens a command encoder and backbuffer view; scenes
// draw into them through the 3D renderer it provides (SceneServices::
// renderer3D), or record their own passes via currentEncoder() (the GPU voxel
// prototype). It has no 2D quad or text renderer, so those services are absent.
//
// It also hosts Dear ImGui with docking and multi-viewport enabled: panels dock
// into the main window or drag out into their own OS windows.
class WebGpuRenderBackend : public IRenderBackend
{
public:
  WebGpuRenderBackend();
  ~WebGpuRenderBackend() override;

  bool init(const char* title, int width, int height) override;
  SDL_Window* window() const override { return m_window; }
  void onResize(int width, int height) override;
  SceneServices* sceneServices() override;

  void beginFrame(int width, int height) override;
  void endFrame() override;

  void imguiProcessEvent(const SDL_Event& event) override;
  void imguiNewFrame() override;
  void imguiRenderDrawData() override;
  bool imguiAvailable() const override { return m_imguiReady; }

  /**
   * Render the 3D scene into `target` instead of the window (e.g. an editor's
   * viewport panel), or back to the window when null. The window is then
   * cleared each frame and only ImGui draws on it. The target must outlive
   * its use here.
   */
  void setSceneTarget(WebGpuRenderTarget* target) { m_sceneTarget = target; }

  void shutdown() override;

  // Reached by systems that record their own WebGPU passes (the GPU voxel
  // world).
  WebGpuContext& context() { return m_context; }
  WGPUCommandEncoder currentEncoder() const { return m_encoder; }
  WGPUTextureView currentView() const { return m_view; }
  int width() const { return m_width; }
  int height() const { return m_height; }

private:
  bool initImGui();
  void shutdownImGui();
  void clearWindow();

  SDL_Window* m_window = nullptr;
  WebGpuContext m_context;
  int m_width = 0;
  int m_height = 0;

  WGPUSurfaceTexture m_surfTex = {};
  WGPUTextureView m_view = nullptr;
  WGPUCommandEncoder m_encoder = nullptr;

  std::unique_ptr<AssetStore> m_assetStore;
  std::unique_ptr<WebGpuRenderer3D> m_renderer3D;
  std::optional<SceneServices> m_services;

  WebGpuRenderTarget* m_sceneTarget = nullptr;

  bool m_imguiReady = false;
  // Set when this frame drew ImGui, so endFrame renders the detached panels.
  bool m_imguiRendered = false;
};

} // namespace sfs
