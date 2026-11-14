#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/camera.h"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * Picks what is under the mouse cursor through the camera it is on, as
 * Unity's PhysicsRaycaster does for its camera. A StandardScene fills it on
 * the scene camera's entity each frame, right after its UI has taken the
 * mouse and before any game object sees input, so a click is picked in the
 * frame it happens, from the view as last drawn. Pickable things are those
 * with a PickableComponent (see pickPickable()).
 *
 * @param bool hover - pick every frame (the thing under the cursor); off, it
 *        picks only on frames a mouse button is pressed
 */
struct CursorPickComponent
{
  bool hover = false;

  /** The world-space ray through the cursor, set every frame. */
  [[sfs::hide]] Ray ray;
  /** The UI has the mouse this frame: nothing is picked. */
  [[sfs::hide]] bool blocked = false;
  /** The cursor is inside the view. */
  [[sfs::hide]] bool inView = false;
  /**
   * The pickable entity picked this frame (with a press, or every frame with
   * `hover`); invalid when nothing was.
   */
  [[sfs::hide]] Entity picked;
  /** The mesh entity the ray hit (`picked` or one of its descendants). */
  [[sfs::hide]] Entity part;
  /** Where the ray hit it, in world space. */
  [[sfs::hide]] glm::vec3 point{0.0f};
  /** How far along the ray it was hit. */
  [[sfs::hide]] float distance = 0.0f;
};

} // namespace sfs
