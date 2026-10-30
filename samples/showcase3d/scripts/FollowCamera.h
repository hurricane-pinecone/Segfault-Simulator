#pragma once
#include <sfs/sfs.h>

#include "glm/glm/gtc/quaternion.hpp"

#include <algorithm>
#include <cmath>

// A third-person camera behind the hero: it swings round to look the way the
// hero faces. Drag with the mouse (either button) to look around; let go and
// it settles back behind. The wheel comes closer or backs off. It keeps out of
// the ground and walls, and focuses the camera's depth of field on the hero.
class FollowCamera : public sfs::GameObject
{
public:
  // How far behind, and the point on the hero it looks at.
  [[sfs::range(2, 30)]] float distance = 8.5f;
  [[sfs::range(1, 10)]] float minDistance = 3.0f;
  [[sfs::range(5, 40)]] float maxDistance = 18.0f;
  [[sfs::range(0, 4)]] float lookHeight = 1.4f;
  // Looking around: radians per pixel dragged.
  [[sfs::range(0.001, 0.02)]] float dragSensitivity = 0.006f;
  // How quickly it catches up (higher: snappier).
  [[sfs::range(1, 30)]] float smoothing = 9.0f;
  // How quickly it swings round behind the hero (and back after a look
  // around).
  [[sfs::range(0.5, 20)]] float followRate = 6.0f;
  // The angle it looks down at when it settles back.
  [[sfs::range(0, 1.2)]] float restingPitch = 0.38f;

  /** Which way it looks across the ground (radians around Z, 0 = +X). */
  float yaw() const { return m_yaw; }

  /** Shake the view (a thump: 0.1 small, 0.4 big); it settles by itself. */
  void shake(float amount) { m_shake = std::max(m_shake, amount); }

protected:
  void onCreate(sfs::Scene& scene) override
  {
    m_hero = scene.findEntity("Hero");
    m_heroModel = scene.findEntity("Hero/Model");
    m_physics = scene.tryGetSystem<sfs::PhysicsSystem3D>();
    // Start behind the hero.
    m_yaw = heroFacing();
    m_pitch = restingPitch;
    if (m_hero.isValid())
    {
      m_focus = heroFocus();
      m_eye = desiredEye(distance);
    }
  }

  void onProcessInput(const sfs::Input& input) override
  {
    const sfs::MouseInput& mouse = input.mouse();
    m_dragging = mouse.mouseHeld(sfs::MouseButton::Left) ||
                 mouse.mouseHeld(sfs::MouseButton::Right);
    if (m_dragging)
    {
      const glm::vec2 delta = mouse.getDelta();
      m_yaw -= delta.x * dragSensitivity;
      m_pitch = std::clamp(m_pitch + delta.y * dragSensitivity, -0.12f, 1.2f);
    }
    if (const int scroll = mouse.getScrollY(); scroll != 0)
      distance =
          std::clamp(distance * std::pow(0.88f, static_cast<float>(scroll)),
                     minDistance,
                     maxDistance);
  }

  void onUpdate(double deltaTime) override
  {
    if (!m_hero.isValid() || !entity().hasComponent<sfs::TransformComponent>())
      return;
    const float dt = static_cast<float>(deltaTime);

    // Unless you're looking around, it swings round behind the hero.
    if (!m_dragging)
    {
      const float settle = 1.0f - std::exp(-followRate * dt);
      m_yaw += std::remainder(heroFacing() - m_yaw, 6.2831853f) * settle;
      m_pitch += (restingPitch - m_pitch) * settle;
    }

    const float ease = 1.0f - std::exp(-smoothing * dt);
    m_focus += (heroFocus() - m_focus) * ease;

    // Back off as far as asked, but not through the ground or a wall.
    float reach = distance;
    if (m_physics)
    {
      const glm::vec3 back = desiredEye(1.0f) - m_focus;
      sfs::Ray ray;
      ray.origin = m_focus + glm::vec3(0.0f, 0.0f, 0.6f); // clear of the hero
      ray.direction = glm::normalize(back);
      if (const auto hit = m_physics->raycast(ray, distance))
        reach = std::max(hit->distance - 0.35f, 1.2f);
    }
    const glm::vec3 wanted = desiredEye(reach);
    m_eye += (wanted - m_eye) * ease;
    m_eye.z = std::max(m_eye.z, 0.35f);

    // A shake: a jitter that dies away.
    m_shake = std::max(m_shake - dt * 1.6f, 0.0f);
    m_shakeClock += dt;
    const glm::vec3 jitter = glm::vec3(std::sin(m_shakeClock * 61.0f),
                                       std::sin(m_shakeClock * 47.0f + 1.3f),
                                       std::sin(m_shakeClock * 53.0f + 2.1f)) *
                             (m_shake * m_shake);

    auto& transform = entity().getComponent<sfs::TransformComponent>();
    transform.position = m_eye + jitter;
    transform.rotation = lookRotation(m_focus - m_eye);
    if (entity().hasComponent<sfs::CameraComponent>())
      entity().getComponent<sfs::CameraComponent>().focusDistance =
          std::max(glm::length(m_focus - m_eye), 0.5f);
  }

private:
  // Which way the hero faces across the ground (its model's turn).
  float heroFacing() const
  {
    if (!m_heroModel.isValid() ||
        !m_heroModel.hasComponent<sfs::TransformComponent>())
      return 1.5707963f;
    const glm::vec3 forward =
        m_heroModel.getComponent<sfs::TransformComponent>().rotation *
        glm::vec3(1.0f, 0.0f, 0.0f);
    return std::atan2(forward.y, forward.x);
  }

  glm::vec3 heroFocus() const
  {
    return m_hero.getComponent<sfs::TransformComponent>().position +
           glm::vec3(0.0f, 0.0f, lookHeight);
  }

  // Where the eye goes `reach` metres back from the focus.
  glm::vec3 desiredEye(float reach) const
  {
    const glm::vec3 forward(std::cos(m_pitch) * std::cos(m_yaw),
                            std::cos(m_pitch) * std::sin(m_yaw),
                            -std::sin(m_pitch));
    return m_focus - forward * reach;
  }

  // A camera entity looks along its +X with +Z up (CameraComponent::applyTo).
  static glm::quat lookRotation(const glm::vec3& direction)
  {
    const glm::vec3 x = glm::normalize(direction);
    glm::vec3 y = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), x);
    y = glm::length(y) > 1e-4f ? glm::normalize(y)
                               : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 z = glm::cross(x, y);
    return glm::quat_cast(glm::mat3(x, y, z));
  }

  sfs::Entity m_hero;
  sfs::Entity m_heroModel;
  sfs::PhysicsSystem3D* m_physics = nullptr;
  float m_yaw = 0.0f;
  float m_pitch = 0.4f;
  bool m_dragging = false;
  glm::vec3 m_focus{0.0f};
  glm::vec3 m_eye{0.0f};
  float m_shake = 0.0f;
  float m_shakeClock = 0.0f;
};
