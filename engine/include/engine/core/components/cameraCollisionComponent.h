#pragma once

#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/gtc/quaternion.hpp"

namespace sfs
{

/**
 * Keeps a camera's view of what it looks at clear of solid things, as Unity
 * Cinemachine's Deoccluder does when it preserves the camera's distance: where
 * something stands between the eye and the pivot (the point focusDistance
 * ahead, CameraComponent), the camera pitches up about the pivot until its
 * line to the pivot passes over it, rather than pulling in. It goes up at
 * once and eases back down once clear. Put it on a camera entity (with a
 * CameraComponent) that a script or a follow moves: CameraSystem lifts that
 * pose each frame, after every game object's update.
 *
 * What blocks: static colliders (BoxCollider3DComponent,
 * SphereCollider3DComponent, CapsuleCollider3DComponent on entities with no
 * RigidBody3DComponent) within reach of the view, by their own shapes.
 * Triggers never block. The line is tested at `samples` points along the
 * ground from the pivot back to under the eye; each wants it to pass over
 * the top of what stands there by a clearance that grows from
 * targetClearance at the pivot to eyeClearance at the eye.
 *
 * @param int layers - the collider layers that block, as a bit mask (bit n:
 *        layer n; -1: all)
 * @param float minHeight - blockers less tall than this are seen over
 * @param float targetClearance - how far the line keeps above blockers at the
 *        pivot end, in metres
 * @param float eyeClearance - how far it keeps above them at the eye
 * @param int samples - how many points along the line are tested
 * @param float maxPitch - the steepest it ever looks down, in degrees below
 *        level: a lift never takes it past this
 * @param float easeBack - how quickly the lift eases back down once clear,
 *        per second
 * @param bool movingBlocks - bodies with a RigidBody3DComponent block too
 * @param bool enabled - whether it lifts the camera now: off, it leaves the
 *        controller's pose as it is and its lift eases back as when nothing
 *        blocks (a script turns it off for a view that needs none, such as a
 *        driver's seat)
 */
struct CameraCollisionComponent
{
  int layers = -1;
  [[sfs::range(0, 20)]] float minHeight = 1.0f;
  [[sfs::range(0, 10)]] float targetClearance = 0.2f;
  [[sfs::range(0, 10)]] float eyeClearance = 2.0f;
  [[sfs::range(1, 50)]] int samples = 10;
  [[sfs::range(0, 90)]] float maxPitch = 80.0f;
  [[sfs::range(0.1, 20)]] float easeBack = 3.0f;
  bool movingBlocks = false;
  bool enabled = true;

  /** The pitch it adds now, in radians (CameraSystem's). */
  [[sfs::hide]] float lift = 0.0f;
  /** It has written a pose: the written and lifted-from poses are set. */
  [[sfs::hide]] bool wrote = false;
  /** The local pose it last left the camera in. */
  [[sfs::hide]] glm::vec3 writtenPosition{0.0f};
  [[sfs::hide]] glm::quat writtenRotation{1.0f, 0.0f, 0.0f, 0.0f};
  /**
   * The local pose it lifted from: the controller's, which it lifts again
   * when nothing has moved the camera since.
   */
  [[sfs::hide]] glm::vec3 fromPosition{0.0f};
  [[sfs::hide]] glm::quat fromRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace sfs
