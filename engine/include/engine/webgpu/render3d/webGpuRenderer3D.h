#pragma once

#include "engine/runtime/rendering3d/iRenderer3D.h"
#include "engine/webgpu/wgpuHandle.h"

#include <webgpu/webgpu.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace sfs
{

class WebGpuContext;

/**
 * IRenderer3D on WebGPU: a forward renderer with a sun shadow map.
 *
 * endFrame() records two passes into the backend's command encoder: the
 * shadow pass (depth-only, every shadow-casting instance from the sun's view)
 * then the lit pass (clears to the sky colour, draws every visible instance
 * with the shared lighting model). Instances of a mesh draw in one instanced
 * call per pass; all of a frame's instance data goes up in a single write.
 *
 * Owned by WebGpuRenderBackend, which points it at each frame's encoder and
 * backbuffer with setTarget().
 */
class WebGpuRenderer3D final : public IRenderer3D
{
public:
  explicit WebGpuRenderer3D(WebGpuContext& context,
                            uint32_t shadowMapResolution = 2048);
  ~WebGpuRenderer3D() override;

  WebGpuRenderer3D(const WebGpuRenderer3D&) = delete;
  WebGpuRenderer3D& operator=(const WebGpuRenderer3D&) = delete;

  /**
   * Build pipelines and the shadow map. Call once before use.
   *
   * @return false (logged) if a GPU resource failed to build.
   */
  bool initialize();

  /**
   * Where this frame renders. A null encoder or view (e.g. the surface was
   * lost) makes endFrame() record nothing.
   */
  void setTarget(WGPUCommandEncoder encoder,
                 WGPUTextureView target,
                 glm::ivec2 size);

  MeshHandle createMesh(const MeshData& mesh) override;
  void destroyMesh(MeshHandle mesh) override;
  glm::ivec2 viewportSize() const override { return m_viewport; }
  uint32_t shadowMapResolution() const override { return m_shadowResolution; }
  void beginFrame(const FrameParams3D& frame) override;
  void drawMesh(const MeshBatch3D& batch) override;
  void endFrame() override;

private:
  struct GpuMesh
  {
    wgpu::Buffer vertices;
    wgpu::Buffer indices;
    uint32_t indexCount = 0;
  };

  // One instanced draw: a mesh and its run of instances in m_instances.
  struct Draw
  {
    uint32_t meshSlot = 0;
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
  };

  bool createPipelines();
  void createShadowMap();
  void ensureDepthTarget();
  void ensureInstanceCapacity(std::size_t instanceCount);
  void uploadFrame();
  void encodeShadowPass();
  void encodeLitPass();
  void encodeDraws(WGPURenderPassEncoder pass, const std::vector<Draw>& draws);

  const GpuMesh* findMesh(MeshHandle mesh) const;

  WebGpuContext& m_context;
  uint32_t m_shadowResolution;

  // Mesh slots; a handle's id is its slot index + 1 (0 stays the null handle).
  std::vector<std::optional<GpuMesh>> m_meshes;
  std::vector<uint32_t> m_freeSlots;

  wgpu::Buffer m_frameUniforms;
  wgpu::Buffer m_instanceBuffer;
  std::size_t m_instanceCapacity = 0;

  wgpu::Texture m_shadowMap;
  wgpu::TextureView m_shadowView;
  wgpu::Sampler m_shadowSampler;

  wgpu::Texture m_depth;
  wgpu::TextureView m_depthView;
  glm::ivec2 m_depthSize{0, 0};

  wgpu::BindGroupLayout m_shadowLayout;
  wgpu::BindGroupLayout m_litLayout;
  wgpu::BindGroup m_shadowBindings;
  wgpu::BindGroup m_litBindings;
  wgpu::RenderPipeline m_shadowPipeline;
  wgpu::RenderPipeline m_litPipeline;

  // This frame's target, set by the backend.
  WGPUCommandEncoder m_encoder = nullptr;
  WGPUTextureView m_target = nullptr;
  glm::ivec2 m_viewport{0, 0};

  // This frame's work, gathered between beginFrame() and endFrame().
  bool m_frameOpen = false;
  FrameParams3D m_frame;
  std::vector<PointLight3D> m_pointLights; // m_frame.pointLights copied out
  std::vector<MeshInstance> m_instances;
  std::vector<Draw> m_litDraws;
  std::vector<Draw> m_shadowDraws;
};

} // namespace sfs
