#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{
struct Camera3D;
}

// Editor viewport navigation for a Camera3D, in the style of DCC tools:
// right-drag orbits, middle-drag (or shift+right-drag) pans, the wheel zooms
// toward the target, and focus() frames a point. Driven by the viewport panel
// only while the mouse is over it.
class ViewportCamera
{
public:
  struct Input
  {
    glm::vec2 mouseDelta{0.0f}; // pixels this frame
    float wheel = 0.0f;         // notches this frame
    bool orbit = false;         // right button held
    bool pan = false;           // middle button (or shift+right) held
  };

  /** Apply one frame of input. `viewportHeight` scales panning to pixels. */
  void apply(sfs::Camera3D& camera, const Input& input, float viewportHeight) const;

  /** Re-target the camera on `point`, keeping its angle. */
  void focus(sfs::Camera3D& camera, const glm::vec3& point) const;
};
