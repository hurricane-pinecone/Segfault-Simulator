#pragma once

#include <glm/glm/common.hpp>
#include <glm/glm/exponential.hpp>
#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <glm/glm/trigonometric.hpp>

namespace sfs
{

/**
 * Makes a camera entity follow another and shake. CameraSystem eases the
 * entity's TransformComponent toward the target's position + offset (rate set
 * by smoothing); call shake() for a screen shake. Works the same in every
 * render style: a flat or isometric camera centres on the followed point, and
 * a 3D one moves with it, keeping its own orientation.
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

  /**
   * Start a screen shake, restarting any in progress.
   *
   * @param float strength - normalised 0..1 (clamped), scaled by shakeMaxOffset
   * @param float duration - length in seconds, default 0.5
   * @param float decay - fade-curve exponent over the duration (1 = linear,
   *                       >1 = fast initial drop then tail), default 1
   */
  void shake(float strength, float duration = 0.5f, float decay = 1.0f)
  {
    m_shake.trigger(strength, duration, decay);
  }

  // Engine-internal: CameraSystem advances the shake each frame and the render
  // systems read the offset (across the view). Games use shake() above.
  void updateShake(float dt) { m_shake.update(dt, shakeMaxOffset); }
  glm::vec2 shakeOffset() const { return m_shake.offset(); }

private:
  // Shake animator. Amplitude rides a (1 - elapsed/duration)^decay envelope;
  // two detuned oscillators give an elliptical wobble rather than a
  // straight-line jitter. Private so it's not part of the engine's component
  // API -- the shake is reached only through CameraFollowComponent's methods.
  class Shake
  {
  public:
    void trigger(float strength, float duration, float decay)
    {
      m_strength = glm::clamp(strength, 0.0f, 1.0f);
      m_duration = glm::max(duration, 0.0001f);
      m_decay = glm::max(decay, 0.0f);
      m_elapsed = 0.0f;
    }

    void update(float dt, float maxOffset)
    {
      if (m_strength <= 0.0f)
      {
        m_offset = {0.0f, 0.0f};
        return;
      }

      m_elapsed += dt;

      if (m_elapsed >= m_duration)
      {
        m_strength = 0.0f;
        m_offset = {0.0f, 0.0f};
        return;
      }

      float k = 1.0f - m_elapsed / m_duration;
      float amplitude = m_strength * maxOffset * glm::pow(k, m_decay);

      m_offset = {glm::sin(m_elapsed * 73.0f) * amplitude,
                  glm::cos(m_elapsed * 91.0f) * amplitude};
    }

    glm::vec2 offset() const { return m_offset; }

  private:
    glm::vec2 m_offset{0.0f, 0.0f};
    float m_strength = 0.0f;
    float m_duration = 0.0f;
    float m_decay = 1.0f;
    float m_elapsed = 0.0f;
  };

  Shake m_shake;
};

} // namespace sfs
