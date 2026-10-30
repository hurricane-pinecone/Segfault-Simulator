#pragma once

#include "engine/core/rendering/aabb.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{
struct Camera;
}

// Editor viewport navigation for a Camera, in the style of DCC tools:
// right-drag orbits, middle-drag (or shift+right-drag) pans, the wheel zooms
// toward the target, and focus() frames an object. Driven by the viewport panel
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
    // A 2D game's view: straight down at its plane, orthographic. Orbiting
    // pans instead, and the wheel zooms over a pixel world's range.
    bool planar = false;
  };

  /** Point `camera` straight down at the plane, orthographic (a 2D game's). */
  static void makePlanar(sfs::Camera& camera);
  /** Back to a 3D view: looking down at an angle, in perspective. */
  static void makeAngled(sfs::Camera& camera);

  /** Apply one frame of input. `viewportHeight` scales panning to pixels. */
  void apply(sfs::Camera& camera, const Input& input, float viewportHeight) const;

  /**
   * Frame a box: the camera turns its target to the box's centre and moves in
   * or out until the whole box fits, keeping its angle.
   */
  void focus(sfs::Camera& camera, const sfs::Aabb& bounds) const;
};
