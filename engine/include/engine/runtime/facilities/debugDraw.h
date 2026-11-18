#pragma once

#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/core/rendering/debugLines.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/sceneManager/scene.h"

#include <glm/glm/ext/matrix_float4x4.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <glm/glm/ext/vector_float4.hpp>

#include <cstdint>
#include <optional>

namespace sfs
{

class RenderSystem;

namespace facility
{

/**
 * Debug drawing, as game code uses it (`debugDraw()` on a System, Service,
 * GameObject or Scene): lines and shapes drawn over this frame (a route, a
 * ray, what blocks a way), and the colliders forced on or off. A debug
 * feature: it draws only while debugFeatures::enabled() (the editor, Debug
 * builds and development exports), and a release export compiles its calls
 * away. Off, or in a scene that doesn't render, it's silent: no warning.
 *
 * A value handle over the scene's RenderSystem's DebugLines, valid for the
 * step that got it, on the scene's thread only: get it where it's used and
 * keep it in no member. The frame draws what was added and empties it, so
 * draw every frame to keep a shape up: a Service, with no update, shows a
 * line for one frame. Shapes are placed by `placed`, a rigid matrix (turn and
 * move, no scale): scale the sizes instead.
 */
class DebugDraw
{
public:
  /**
   * A box's 12 edges.
   * @param placed where it is
   * @param centre its centre, in placed's frame
   * @param half half its size, along placed's axes
   * @param color its colour (pack())
   */
  inline void box(const glm::mat4& placed,
                  const glm::vec3& centre,
                  const glm::vec3& half,
                  uint32_t color) const
  {
    if constexpr (debugFeatures::compiled())
      if (m_lines)
        m_lines->box(placed, centre, half, color);
  }

  /**
   * A capsule along placed's z: its end circles, the lines joining them and
   * its rounded ends.
   * @param placed where it is
   * @param centre its centre, in placed's frame
   * @param radius its radius
   * @param halfLength from its centre to each end circle's
   * @param color its colour (pack())
   */
  inline void capsule(const glm::mat4& placed,
                      const glm::vec3& centre,
                      float radius,
                      float halfLength,
                      uint32_t color) const
  {
    if constexpr (debugFeatures::compiled())
      if (m_lines)
        m_lines->capsule(placed, centre, radius, halfLength, color);
  }

  /**
   * A circle in the plane of unit axes u and v, or the arc of it from
   * `start` to `end` turns (0..1) from u toward v.
   * @param placed where it is
   * @param centre its centre, in placed's frame
   * @param u one axis of its plane
   * @param v the other
   * @param radius its radius
   * @param color its colour (pack())
   * @param start where the arc starts, in turns
   * @param end where it ends
   */
  inline void circle(const glm::mat4& placed,
                     const glm::vec3& centre,
                     const glm::vec3& u,
                     const glm::vec3& v,
                     float radius,
                     uint32_t color,
                     float start = 0.0f,
                     float end = 1.0f) const
  {
    if constexpr (debugFeatures::compiled())
      if (m_lines)
        m_lines->circle(placed, centre, u, v, radius, color, start, end);
  }

  /** @return whether it draws: debug features on, and a scene that renders */
  inline bool enabled() const
  {
    return debugFeatures::compiled() && m_lines != nullptr;
  }

  /**
   * Forces drawing every collider on or off while the game runs, over the
   * scene's Draw Colliders setting (a development key).
   * @param on forced on or off; none: the scene's setting again
   */
  void forceColliders(std::optional<bool> on) const;

  /**
   * The debug drawing of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade; off while debug features are, or with no RenderSystem
   */
  static DebugDraw in(Registry* registry);

  /**
   * A line in world space.
   * @param from one end
   * @param to the other
   * @param color its colour (pack())
   */
  inline void
  line(const glm::vec3& from, const glm::vec3& to, uint32_t color) const
  {
    if constexpr (debugFeatures::compiled())
      if (m_lines)
        m_lines->line(from, to, color);
  }

  /**
   * A line in world space.
   * @param from one end
   * @param to the other
   * @param color its colour (0..1, as seen, and alpha)
   */
  inline void
  line(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color) const
  {
    if constexpr (debugFeatures::compiled())
      if (m_lines)
        m_lines->line(from, to, color);
  }

  /**
   * A colour packed for drawing.
   * @param color 0..1, as seen, and alpha
   * @return it packed
   */
  inline static uint32_t pack(const glm::vec4& color)
  {
    return DebugLines::pack(color);
  }

  /**
   * A sphere as three circles, one round each axis.
   * @param placed where it is
   * @param centre its centre, in placed's frame
   * @param radius its radius
   * @param color its colour (pack())
   */
  inline void sphere(const glm::mat4& placed,
                     const glm::vec3& centre,
                     float radius,
                     uint32_t color) const
  {
    if constexpr (debugFeatures::compiled())
      if (m_lines)
        m_lines->sphere(placed, centre, radius, color);
  }

private:
  inline DebugDraw(DebugLines* lines, RenderSystem* render)
      : m_lines(lines), m_render(render)
  {
  }

  // The frame's lines and the RenderSystem keeping them; both null while
  // debug features are off.
  DebugLines* m_lines = nullptr;
  RenderSystem* m_render = nullptr;
};

} // namespace facility

inline facility::DebugDraw System::debugDraw() const
{
  return facility::DebugDraw::in(registry);
}

inline facility::DebugDraw Service::debugDraw() const
{
  return facility::DebugDraw::in(m_registry);
}

inline facility::DebugDraw GameObject::debugDraw() const
{
  return m_scene ? m_scene->debugDraw() : facility::DebugDraw::in(nullptr);
}

inline facility::DebugDraw Scene::debugDraw()
{
  return facility::DebugDraw::in(&m_registry);
}

} // namespace sfs
