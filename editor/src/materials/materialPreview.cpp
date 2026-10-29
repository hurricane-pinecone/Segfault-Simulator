#include "materials/materialPreview.h"

#include "engine/core/logger/logger.h"
#include "engine/core/rendering/3d/camera3D.h"
#include "engine/core/rendering/3d/meshPrimitives.h"
#include "engine/webgpu/webGpuContext.h"

#include "glm/glm/geometric.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

MaterialPreview::MaterialPreview(sfs::WebGpuContext& gpu)
    : m_gpu(gpu), m_renderer(gpu, 256), m_target(gpu)
{
}

bool MaterialPreview::ensureReady()
{
  if (m_initialized || m_failed)
    return m_initialized;
  if (!m_renderer.initialize())
  {
    LOG_ERROR("The material preview can't draw");
    m_failed = true;
    return false;
  }
  // Each shape around the origin's up axis, about 2 across.
  const sfs::MeshData shapes[] = {sfs::meshes::sphere(1.0f, 48, 24),
                                  sfs::meshes::box(glm::vec3(1.6f)),
                                  sfs::meshes::plane(glm::vec2(2.4f))};
  for (std::size_t i = 0; i < m_meshes.size(); ++i)
  {
    m_meshes[i] = m_renderer.createMesh(shapes[i]);
    m_centres[i] = shapes[i].bounds().center();
    for (const sfs::MeshVertex& vertex : shapes[i].vertices)
      m_radii[i] =
          std::max(m_radii[i], glm::length(vertex.position - m_centres[i]));
  }
  m_initialized = true;
  return true;
}

void MaterialPreview::setMaterial(const sfs::MeshMaterial& material)
{
  if (!ensureReady())
    return;
  if (m_material.valid())
    m_renderer.updateMaterial(m_material, material);
  else
    m_material = m_renderer.createMaterial(material);
}

void MaterialPreview::render(glm::ivec2 size, float time)
{
  m_target.resize(size);
  if (!m_target.valid())
    return;

  // Orbiting the shape's middle, far enough back to fit it.
  const float radius = std::max(m_radii[static_cast<int>(m_shape)], 0.5f);
  sfs::Camera3D camera;
  camera.target = m_centres[static_cast<int>(m_shape)];
  camera.yaw = m_yaw;
  camera.pitch = m_pitch;
  camera.fieldOfView = 0.6f;
  camera.distance = radius / std::sin(camera.fieldOfView * 0.5f) * 1.02f;
  camera.nearPlane = camera.distance * 0.05f;
  camera.farPlane = camera.distance * 4.0f;

  sfs::FrameParams3D frame;
  frame.cameraPosition = camera.position();
  frame.view = camera.viewMatrix();
  frame.projection = camera.projectionMatrix(static_cast<float>(size.x) /
                                             static_cast<float>(size.y));
  frame.viewProjection = frame.projection * frame.view;
  frame.cameraForward = camera.viewDirection();
  frame.nearPlane = camera.nearPlane;
  frame.farPlane = camera.farPlane;
  frame.shadows.enabled = false; // nothing to cast onto
  frame.time = time;

  const WGPUCommandEncoder encoder =
      wgpuDeviceCreateCommandEncoder(m_gpu.device(), nullptr);
  m_renderer.setTarget(encoder, m_target.view(), size);
  m_renderer.beginFrame(frame);
  const sfs::MeshInstance instance;
  const sfs::MaterialHandle materials[] = {m_material};
  sfs::MeshBatch3D batch;
  batch.mesh = m_meshes[static_cast<int>(m_shape)];
  batch.materials = materials;
  batch.instances = {&instance, 1};
  m_renderer.drawMesh(batch);
  m_renderer.endFrame();
  const WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(m_gpu.queue(), 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);
}

void MaterialPreview::draw(float size, float time)
{
  const ImVec2 extent(size, size);
  if (!ensureReady() || !m_material.valid() || size < 8.0f)
  {
    ImGui::Dummy(extent);
    return;
  }
  // Pixels, not points, on a high-DPI display.
  const float scale = ImGui::GetWindowDpiScale();
  const auto pixels =
      static_cast<int>(std::lround(size * std::max(scale, 1.0f)));
  render({pixels, pixels}, time);

  // A button under the image, so dragging it turns the shape.
  ImGui::InvisibleButton("##preview", extent);
  const ImVec2 min = ImGui::GetItemRectMin();
  ImGui::GetWindowDrawList()->AddImage(
      static_cast<ImTextureID>(
          reinterpret_cast<intptr_t>(m_target.imguiView())),
      min,
      ImVec2(min.x + size, min.y + size));
  ImGui::SetItemTooltip("Drag to turn it");
  if (ImGui::IsItemActive())
  {
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    m_yaw -= delta.x * 0.01f;
    m_pitch = std::clamp(m_pitch + delta.y * 0.01f, -1.4f, 1.4f);
  }
}
