#pragma once

#include "engine/core/rendering/mesh.h"
#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/rendering/iRenderer.h"

#include <array>
#include <memory>

// A material drawn by the real renderer on a sphere, cube or plane, for the
// Material Editor: exactly as the scene draws it, Time running. It has its own
// renderer (from the editor's backend) and target, so it draws whatever the
// scene is doing.
class MaterialPreview
{
public:
  enum class Shape
  {
    Sphere,
    Cube,
    Plane,
  };

  explicit MaterialPreview(sfs::IRenderBackend& backend);

  /** Draw with this material from now on. */
  void setMaterial(const sfs::MeshMaterial& material);

  /**
   * Draw the preview `size` across (pixels) and show it as an ImGui image.
   * Dragging the image turns the shape. `time` is the seconds Time nodes
   * read.
   */
  void draw(float size, float time);

  Shape shape() const { return m_shape; }
  void setShape(Shape shape) { m_shape = shape; }

private:
  bool ensureReady();
  void render(glm::ivec2 size, float time);

  sfs::IRenderBackend& m_backend;
  std::unique_ptr<sfs::IRenderer> m_renderer;
  std::unique_ptr<sfs::IRenderTarget> m_target;
  bool m_initialized = false;
  bool m_failed = false;
  std::array<sfs::MeshHandle, 3> m_meshes{}; // by Shape
  std::array<glm::vec3, 3> m_centres{};      // by Shape: its bounds' middle
  std::array<float, 3> m_radii{}; // and how far its farthest point is
  sfs::MaterialHandle m_material;
  Shape m_shape = Shape::Sphere;
  float m_yaw = 0.6f;
  float m_pitch = 0.35f;
};
