#pragma once

#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/webgpu/nullQuadRenderer.h"
#include "engine/webgpu/webGpuContext.h"

#include <webgpu/webgpu.h>

#include <memory>
#include <optional>

namespace sfs
{

class AssetStore;
class TextRenderer;

// WebGPU render backend: an SDL window (no GL context) plus a WebGPU device and
// surface. Draws nothing itself -- it opens the per-frame command encoder and
// backbuffer view that a voxel render system records its compute + raymarch
// passes into. The SceneServices it offers are unused placeholders (the voxel
// scene draws only through WebGPU), so ImGui and the GL content stack are
// absent.
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

  void shutdown() override;

  // Reached by the voxel render system (via the scene) to build pipelines and
  // record passes.
  WebGpuContext& context() { return m_context; }
  WGPUCommandEncoder currentEncoder() const { return m_encoder; }
  WGPUTextureView currentView() const { return m_view; }
  int width() const { return m_width; }
  int height() const { return m_height; }

private:
  SDL_Window* m_window = nullptr;
  WebGpuContext m_context;
  int m_width = 0;
  int m_height = 0;

  WGPUSurfaceTexture m_surfTex = {};
  WGPUTextureView m_view = nullptr;
  WGPUCommandEncoder m_encoder = nullptr;

  // Unused placeholder services satisfying Scene's SceneServices contract.
  std::unique_ptr<AssetStore> m_assetStore;
  NullQuadRenderer m_nullQuad;
  std::unique_ptr<TextRenderer> m_textRenderer;
  std::optional<SceneServices> m_services;
};

} // namespace sfs
