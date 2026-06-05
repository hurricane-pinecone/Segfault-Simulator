#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

// Small keyframed scalar curve over normalized time t in [0,1]. POD with fixed
// capacity (no allocation) so a future data-driven loader can fill it directly.
// Stops are assumed sorted by t; sampling clamps outside the first/last stop.
struct Curve
{
  static constexpr int MaxStops = 8;

  struct Stop
  {
    float t = 0.0f;
    float value = 0.0f;
  };

  Stop stops[MaxStops] = {};
  int count = 0;

  static Curve constant(float v)
  {
    Curve c;
    c.stops[0] = {0.0f, v};
    c.count = 1;
    return c;
  }

  static Curve linear(float start, float end)
  {
    Curve c;
    c.stops[0] = {0.0f, start};
    c.stops[1] = {1.0f, end};
    c.count = 2;
    return c;
  }

  Curve& add(float t, float value)
  {
    if (count < MaxStops)
      stops[count++] = {t, value};
    return *this;
  }

  float sample(float t) const
  {
    if (count <= 0)
      return 0.0f;
    if (count == 1 || t <= stops[0].t)
      return stops[0].value;
    if (t >= stops[count - 1].t)
      return stops[count - 1].value;

    for (int i = 1; i < count; ++i)
    {
      if (t <= stops[i].t)
      {
        const float span = stops[i].t - stops[i - 1].t;
        const float f = span > 1e-6f ? (t - stops[i - 1].t) / span : 0.0f;
        return stops[i - 1].value + (stops[i].value - stops[i - 1].value) * f;
      }
    }

    return stops[count - 1].value;
  }
};

// Keyframed RGB gradient over normalized time t in [0,1]. Same POD shape as Curve.
struct Gradient
{
  static constexpr int MaxStops = 8;

  struct Stop
  {
    float t = 0.0f;
    glm::vec3 color{1.0f, 1.0f, 1.0f};
  };

  Stop stops[MaxStops] = {};
  int count = 0;

  static Gradient constant(glm::vec3 color)
  {
    Gradient g;
    g.stops[0] = {0.0f, color};
    g.count = 1;
    return g;
  }

  static Gradient twoStop(glm::vec3 start, glm::vec3 end)
  {
    Gradient g;
    g.stops[0] = {0.0f, start};
    g.stops[1] = {1.0f, end};
    g.count = 2;
    return g;
  }

  Gradient& add(float t, glm::vec3 color)
  {
    if (count < MaxStops)
      stops[count++] = {t, color};
    return *this;
  }

  glm::vec3 sample(float t) const
  {
    if (count <= 0)
      return glm::vec3{1.0f};
    if (count == 1 || t <= stops[0].t)
      return stops[0].color;
    if (t >= stops[count - 1].t)
      return stops[count - 1].color;

    for (int i = 1; i < count; ++i)
    {
      if (t <= stops[i].t)
      {
        const float span = stops[i].t - stops[i - 1].t;
        const float f = span > 1e-6f ? (t - stops[i - 1].t) / span : 0.0f;
        return stops[i - 1].color + (stops[i].color - stops[i - 1].color) * f;
      }
    }

    return stops[count - 1].color;
  }
};

} // namespace sfs
