#pragma once

#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sfs
{

/** One end of a debug line: where it is in the world, and its colour. */
struct DebugLineVertex
{
  glm::vec3 position{0.0f};
  uint32_t color = 0xffffffffu; // RGBA8 as seen (sRGB), red in the low byte
};

/**
 * Lines to draw over a frame for debugging (colliders, a route, a ray): a
 * plain list of segments in world space, each end a DebugLineVertex. The
 * renderer draws them all in one pass, depth tested, in their own colours,
 * however many there are (see IRenderer::drawLines). A RenderSystem keeps
 * one a frame (RenderSystem::debugLines()): game code adds to it as it
 * updates, the frame draws it and empties it. A debug feature: drawn only
 * while debugFeatures::enabled().
 *
 * The shapes are placed by `placed`, a rigid matrix (turn and move, no
 * scale): scale the sizes instead, as the physics does.
 */
class DebugLines
{
public:
  /** A colour (0..1, as seen, and alpha) packed for a vertex. */
  static uint32_t pack(const glm::vec4& color);

  void line(const glm::vec3& from, const glm::vec3& to, uint32_t color);
  void line(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color)
  {
    line(from, to, pack(color));
  }

  /** A box's 12 edges: its centre and half its size, along placed's axes. */
  void box(const glm::mat4& placed,
           const glm::vec3& centre,
           const glm::vec3& half,
           uint32_t color);

  /**
   * A circle in the plane of unit axes u and v, or the arc of it from
   * `start` to `end` turns (0..1) from u toward v.
   */
  void circle(const glm::mat4& placed,
              const glm::vec3& centre,
              const glm::vec3& u,
              const glm::vec3& v,
              float radius,
              uint32_t color,
              float start = 0.0f,
              float end = 1.0f);

  /** A sphere as three circles, one around each axis. */
  void sphere(const glm::mat4& placed,
              const glm::vec3& centre,
              float radius,
              uint32_t color);

  /**
   * A capsule along placed's z: its two end circles, four lines joining
   * them and the rounded ends; `halfLength` from its centre to each end
   * circle's (the straight part's half).
   */
  void capsule(const glm::mat4& placed,
               const glm::vec3& centre,
               float radius,
               float halfLength,
               uint32_t color);

  /** Every line's two ends, in order. */
  std::span<const DebugLineVertex> vertices() const { return m_vertices; }
  std::size_t lineCount() const { return m_vertices.size() / 2; }
  bool empty() const { return m_vertices.empty(); }

  /** Drop every line (keeping the room for the next frame's). */
  void clear() { m_vertices.clear(); }

  // Segments a whole circle is drawn with.
  static constexpr int kCircleSegments = 24;

private:
  std::vector<DebugLineVertex> m_vertices;
};

} // namespace sfs
