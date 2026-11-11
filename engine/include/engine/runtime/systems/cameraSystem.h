#pragma once

#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/cameraFollowComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/system.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/runtime/rendering/util/activeCamera.h>
#include <glm/glm/common.hpp>
#include <glm/glm/exponential.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <glm/glm/matrix.hpp>
#include <glm/glm/trigonometric.hpp>

#include <engine/core/ecs/registry.h>

namespace sfs
{

/**
 * Moves cameras that follow something (CameraFollowComponent): eases each one
 * toward its target and advances its shake. The same in every render style.
 * Also resolves the scene's active camera (activeCamera()).
 */
class CameraSystem : public System
{
public:
  CameraSystem()
  {
    registerComponent<TransformComponent>();
    registerComponent<CameraFollowComponent>();
    // The player's view: real time, however fast the game's going.
    setTimeStep(TimeStep::Real);
  }

  void update(double deltaTime) override
  {
    for (auto& cameraEntity : getEntities())
    {
      auto& cameraTransform = cameraEntity.getComponent<TransformComponent>();
      auto& follow = cameraEntity.getComponent<CameraFollowComponent>();

      // Shake is independent of following, so advance it before the target
      // checks below can `continue` past the rest of the loop.
      advanceShake(follow, static_cast<float>(deltaTime));

      auto target = registry->getEntity(follow.target);

      if (!target)
        continue;

      if (!target.hasComponent<TransformComponent>())
        continue;

      // In the world, so a parented camera or target follows as well.
      const glm::vec3 desired =
          hierarchy::worldPosition(target) + follow.offset;
      const glm::vec3 current = hierarchy::worldPosition(cameraEntity);

      float t =
          1.0f - glm::exp(-follow.smoothing * static_cast<float>(deltaTime));

      const glm::vec3 next = glm::mix(current, desired, t);
      cameraTransform.position =
          hierarchy::parentOf(cameraEntity).isValid()
              ? glm::vec3(
                    glm::inverse(hierarchy::parentWorldMatrix(cameraEntity)) *
                    glm::vec4(next, 1.0f))
              : next;
    }
  }

  /**
   * Start a screen shake on a following camera, restarting any in progress.
   *
   * @param float strength - normalised 0..1 (clamped), scaled by its
   *                         shakeMaxOffset
   * @param float duration - length in seconds, default 0.5
   * @param float decay - fade-curve exponent over the duration (1 = linear,
   *                       >1 = fast initial drop then tail), default 1
   */
  static void shake(const Entity& camera,
                    float strength,
                    float duration = 0.5f,
                    float decay = 1.0f)
  {
    if (!camera.isAlive() || !camera.hasComponent<CameraFollowComponent>())
      return;
    auto& follow = camera.getComponent<CameraFollowComponent>();
    follow.shakeStrength = glm::clamp(strength, 0.0f, 1.0f);
    follow.shakeDuration = glm::max(duration, 0.0001f);
    follow.shakeDecay = glm::max(decay, 0.0f);
    follow.shakeElapsed = 0.0f;
  }

  /**
   * A shake's next offset, `dt` seconds on: its amplitude rides a
   * (1 - elapsed/duration)^decay envelope; two detuned oscillators give an
   * elliptical wobble rather than a straight-line jitter.
   */
  static void advanceShake(CameraFollowComponent& follow, float dt)
  {
    if (follow.shakeStrength <= 0.0f)
    {
      follow.shakeOffset = {0.0f, 0.0f};
      return;
    }
    follow.shakeElapsed += dt;
    if (follow.shakeElapsed >= follow.shakeDuration)
    {
      follow.shakeStrength = 0.0f;
      follow.shakeOffset = {0.0f, 0.0f};
      return;
    }
    const float k = 1.0f - follow.shakeElapsed / follow.shakeDuration;
    const float amplitude = follow.shakeStrength * follow.shakeMaxOffset *
                            glm::pow(k, follow.shakeDecay);
    follow.shakeOffset = {glm::sin(follow.shakeElapsed * 73.0f) * amplitude,
                          glm::cos(follow.shakeElapsed * 91.0f) * amplitude};
  }

  // The scene's active camera (see findActiveCamera), or an invalid entity.
  Entity activeCamera() const
  {
    return registry ? findActiveCamera(*registry) : Entity{};
  }
};

} // namespace sfs
