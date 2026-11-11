#pragma once

#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/**
 * Makes a camera entity follow another and shake. CameraSystem eases the
 * entity's TransformComponent toward the target's position + offset (rate set
 * by smoothing); CameraSystem::shake() starts a screen shake. Works the same in
 * every render style: a flat or isometric camera centres on the followed point,
 * and a 3D one moves with it, keeping its own orientation.
 *
 * @param int target - id of the entity to follow (-1: none)
 * @param glm::vec3 offset - offset from the target, default 0
 * @param float smoothing - follow ease rate (higher = snappier), default 8
 */
struct CameraFollowComponent
{
  CameraFollowComponent() = default;
  CameraFollowComponent(int target,
                        glm::vec3 offset = glm::vec3(0.0f),
                        float smoothing = 8.0f)
      : target(target), offset(offset), smoothing(smoothing)
  {
  }

  int target = -1;
  glm::vec3 offset{0.0f};
  [[sfs::range(0, 30)]] float smoothing = 8.0f;

  // Peak shake displacement at strength 1.0, in world units; tune per game.
  [[sfs::range(0, 5)]] float shakeMaxOffset = 0.5f;

  // A screen shake under way (CameraSystem::shake() starts one and advances
  // it; the render systems move the view across itself by `shakeOffset`):
  // its strength (0: none), length, fade exponent and time so far.
  [[sfs::hide]] glm::vec2 shakeOffset{0.0f};
  [[sfs::hide]] float shakeStrength = 0.0f;
  [[sfs::hide]] float shakeDuration = 0.0f;
  [[sfs::hide]] float shakeDecay = 1.0f;
  [[sfs::hide]] float shakeElapsed = 0.0f;
};

} // namespace sfs
