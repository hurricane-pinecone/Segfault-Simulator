#pragma once

#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"
#include "glm/glm/vec3.hpp"

namespace sfs
{

// An orbit camera that looks at the centre of a worldSize^3 voxel volume from a
// fixed elevation, rotating around it by yaw. Packs the 16-float uniform the
// raymarch shader reads, and builds the pick ray for a screen pixel.
class OrbitCamera
{
public:
  explicit OrbitCamera(float worldSize) : m_worldSize(worldSize) {}

  float yaw() const { return m_yaw; }
  void addYaw(float delta) { m_yaw += delta; }

  // Scroll wheel zoom: positive scrollY moves the camera closer to the centre.
  void addZoom(float scrollY)
  {
    m_zoom -= scrollY * 0.1f;
    if (m_zoom < 0.25f)
      m_zoom = 0.25f;
    if (m_zoom > 2.5f)
      m_zoom = 2.5f;
  }

  glm::vec3 position() const
  {
    const glm::vec3 center = centre();
    const float radius = m_worldSize * 0.9f;
    const glm::vec3 offset = {radius * glm::cos(m_yaw),
                              m_worldSize * 0.35f,
                              radius * glm::sin(m_yaw)};
    return center + offset * m_zoom;
  }

  // Fill the 4x vec4 camera uniform: {pos, worldSize}, {forward, width},
  // {rayRight, height}, {rayUp, time}.
  void packUniform(float* out16, int width, int height, float time) const
  {
    const glm::vec3 pos = position();
    const glm::vec3 fwd = glm::normalize(centre() - pos);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
    const glm::vec3 up = glm::cross(right, fwd);
    const float tanHalf = glm::tan(0.5f * 1.04719755f); // 60 deg fov
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const glm::vec3 rRight = right * (tanHalf * aspect);
    const glm::vec3 rUp = up * tanHalf;

    out16[0] = pos.x;
    out16[1] = pos.y;
    out16[2] = pos.z;
    out16[3] = m_worldSize;
    out16[4] = fwd.x;
    out16[5] = fwd.y;
    out16[6] = fwd.z;
    out16[7] = static_cast<float>(width);
    out16[8] = rRight.x;
    out16[9] = rRight.y;
    out16[10] = rRight.z;
    out16[11] = static_cast<float>(height);
    out16[12] = rUp.x;
    out16[13] = rUp.y;
    out16[14] = rUp.z;
    out16[15] = time;
  }

  // Origin + normalised direction of the ray through a screen pixel.
  void pickRay(int width,
               int height,
               float mouseX,
               float mouseY,
               glm::vec3& origin,
               glm::vec3& direction) const
  {
    const glm::vec3 pos = position();
    const glm::vec3 fwd = glm::normalize(centre() - pos);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
    const glm::vec3 up = glm::cross(right, fwd);
    const float tanHalf = glm::tan(0.5f * 1.04719755f);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const glm::vec3 rRight = right * (tanHalf * aspect);
    const glm::vec3 rUp = up * tanHalf;

    const float ndcx = 2.0f * mouseX / static_cast<float>(width) - 1.0f;
    const float ndcy = 1.0f - 2.0f * mouseY / static_cast<float>(height);
    origin = pos;
    direction = glm::normalize(fwd + ndcx * rRight + ndcy * rUp);
  }

private:
  glm::vec3 centre() const
  {
    return {m_worldSize * 0.5f, m_worldSize * 0.2f, m_worldSize * 0.5f};
  }

  float m_worldSize;
  float m_yaw = 0.7f;
  float m_zoom = 1.0f;
};

} // namespace sfs
