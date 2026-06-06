#pragma once

#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

// Swept segment vs. axis-aligned box (slab method). Tests the segment
// `from -> from + seg` against the box [lo, hi]; on a crossing-IN this step it
// sets `tEnter` (the entry parameter along the segment) and `normal` (the entry
// face's outward normal) and returns true. Returns false if the line misses the
// box or starts inside it (tEnter outside [0,1]).
inline bool sweepAabb(glm::vec2 from,
                      glm::vec2 seg,
                      glm::vec2 lo,
                      glm::vec2 hi,
                      float& tEnter,
                      glm::vec2& normal)
{
  float tmin = -1e30f;
  float tmax = 1e30f;
  glm::vec2 nrm{0.0f, 0.0f};

  for (int axis = 0; axis < 2; ++axis)
  {
    const float p = axis == 0 ? from.x : from.y;
    const float d = axis == 0 ? seg.x : seg.y;
    const float l = axis == 0 ? lo.x : lo.y;
    const float h = axis == 0 ? hi.x : hi.y;

    if (glm::abs(d) < 1e-6f)
    {
      if (p < l || p > h)
        return false; // parallel to this slab and outside it
      continue;
    }

    float t1 = (l - p) / d; // low face
    float t2 = (h - p) / d; // high face
    float sign = -1.0f;     // entering the low face -> normal points -axis
    if (t1 > t2)
    {
      const float tmp = t1;
      t1 = t2;
      t2 = tmp;
      sign = 1.0f; // entering the high face -> normal points +axis
    }

    if (t1 > tmin)
    {
      tmin = t1;
      nrm = axis == 0 ? glm::vec2{sign, 0.0f} : glm::vec2{0.0f, sign};
    }
    tmax = glm::min(tmax, t2);
    if (tmin > tmax)
      return false;
  }

  if (tmin < 0.0f || tmin > 1.0f)
    return false; // didn't cross INTO the box this step (or started inside)

  tEnter = tmin;
  normal = nrm;
  return true;
}

} // namespace sfs
