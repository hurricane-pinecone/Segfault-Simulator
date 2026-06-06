#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

// A polygon vertex carried through clipping: a position plus its interpolated UV,
// so a slice gets the right texture coordinate at the cut. The position is in
// whatever 2D space the caller clips in (screen pixels on the flat path, world
// tiles or face along/elevation on the isometric path).
struct ClipVertex
{
  glm::vec2 p{0.0f, 0.0f};
  glm::vec2 uv{0.0f, 0.0f};
};

// Clip a polygon against one axis-aligned half-plane (Sutherland-Hodgman): keep
// the side where coord >= limit (keepGreater) or coord <= limit on `axis`
// (0 = x, 1 = y), interpolating position + UV at each crossing. Returns the new
// vertex count written to `out`.
inline int clipHalfPlane(const ClipVertex* in,
                         int n,
                         int axis,
                         float limit,
                         bool keepGreater,
                         ClipVertex* out)
{
  int m = 0;
  for (int i = 0; i < n; ++i)
  {
    const ClipVertex& a = in[i];
    const ClipVertex& b = in[(i + 1) % n];
    const float ca = axis == 0 ? a.p.x : a.p.y;
    const float cb = axis == 0 ? b.p.x : b.p.y;
    const bool aIn = keepGreater ? ca >= limit : ca <= limit;
    const bool bIn = keepGreater ? cb >= limit : cb <= limit;
    if (aIn)
      out[m++] = a;
    if (aIn != bIn)
    {
      const float denom = cb - ca;
      const float s = denom != 0.0f ? (limit - ca) / denom : 0.0f;
      out[m++] = ClipVertex{a.p + (b.p - a.p) * s, a.uv + (b.uv - a.uv) * s};
    }
  }
  return m;
}

// Clip a convex quad (4 points + UVs) to an axis-aligned rect. Returns the
// clipped convex polygon in `out` (up to 8 vertices); a count < 3 means it was
// fully clipped away. `out` must hold at least 8 ClipVertex.
inline int clipQuadToRect(const glm::vec2* pts,
                          const glm::vec2* uvs,
                          float left,
                          float top,
                          float right,
                          float bottom,
                          ClipVertex* out)
{
  ClipVertex a[12];
  ClipVertex b[12];
  for (int i = 0; i < 4; ++i)
    a[i] = ClipVertex{pts[i], uvs[i]};
  int n = clipHalfPlane(a, 4, 0, left, true, b);
  n = clipHalfPlane(b, n, 0, right, false, a);
  n = clipHalfPlane(a, n, 1, top, true, b);
  n = clipHalfPlane(b, n, 1, bottom, false, out);
  return n;
}

} // namespace sfs
