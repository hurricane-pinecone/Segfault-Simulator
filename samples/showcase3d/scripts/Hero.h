#pragma once
#include <sfs/sfs.h>

#include "FollowCamera.h"
#include "HeroTuning.h"
#include "IslandDirector.h"
#include "components/Spark.h"

#include <algorithm>
#include <cmath>

// The hero: W and S run forward and back, A and D turn it (the camera follows
// round behind), Space jumps (once more in the air), Shift dashes, and it picks
// up sparks by touching them. Its entity is a
// physics body (a capsule that doesn't tip over); its model is a child, turned
// to face the way it runs and squashed as it lands.
//
// Forgiving where it counts: a jump pressed just before landing still happens
// (buffering), and one just after stepping off an edge too (coyote time).
class Hero : public sfs::GameObject
{
public:
  // How it moves (shared data: assets/data/heroTuning.asset).
  sfs::AssetRef<HeroTuning> tuning;
  // Below this, it's fallen in the sea: back to the start.
  [[sfs::range(-20, 0)]] float fallLimit = -3.0f;
  // How fast it backs up with S (a share of its running speed).
  [[sfs::range(0, 1)]] float backSpeed = 0.6f;

protected:
  void onCreate(sfs::Scene& scene) override
  {
    m_physics = scene.tryGetSystem<sfs::PhysicsSystem3D>();
    m_director = scene.tryFindObject<IslandDirector>();
    m_model = scene.findEntity("Hero/Model");
    m_spawn = entity().getComponent<sfs::TransformComponent>().position;
    m_facing = 1.5707963f;
  }

  void onProcessInput(const sfs::Input& input) override
  {
    const sfs::KeyboardInput& keys = input.keyboard();
    const auto held = [&keys](sfs::Key a, sfs::Key b)
    { return keys.keyHeld(a) || keys.keyHeld(b); };
    m_input = {(held(sfs::Key::D, sfs::Key::Right) ? 1.0f : 0.0f) -
                   (held(sfs::Key::A, sfs::Key::Left) ? 1.0f : 0.0f),
               (held(sfs::Key::W, sfs::Key::Up) ? 1.0f : 0.0f) -
                   (held(sfs::Key::S, sfs::Key::Down) ? 1.0f : 0.0f)};
    const bool jump = keys.keyHeld(sfs::Key::Space);
    if (jump && !m_jumpHeld)
      m_jumpQueued = settings().jumpBuffer;
    m_jumpHeld = jump;
    if (keys.keyPressed(sfs::Key::LeftShift) ||
        keys.keyPressed(sfs::Key::RightShift))
      m_dashQueued = true;
    if (keys.keyPressed(sfs::Key::R))
      respawn();
  }

  void onUpdate(double deltaTime) override
  {
    if (!m_physics || !m_physics->hasBody(entity()))
      return;
    const float dt = static_cast<float>(std::min(deltaTime, 0.05));
    const HeroTuning& t = settings();
    auto& transform = entity().getComponent<sfs::TransformComponent>();
    glm::vec3 velocity = m_physics->velocity(entity());

    if (transform.position.z < fallLimit)
    {
      respawn();
      return;
    }

    // On something? A short ray down from just under the feet (the capsule
    // ends at them; a ray starting inside it would find the hero).
    const bool wasGrounded = m_grounded;
    sfs::Ray ray;
    ray.origin = transform.position - glm::vec3(0.0f, 0.0f, 0.02f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    const auto below = m_physics->raycast(ray, 0.15f);
    m_grounded = below && below->entity != entity() && velocity.z < 2.0f;
    if (m_grounded)
    {
      m_coyote = t.coyoteTime;
      m_airJumps = t.airJumps;
      m_dashReady = true;
      if (!wasGrounded && m_fallSpeed < -5.0f)
        land(transform.position, -m_fallSpeed);
    }
    else
      m_coyote = std::max(m_coyote - dt, 0.0f);
    m_fallSpeed = velocity.z;

    // A and D turn it; W and S run along the way it faces (backing up slower).
    m_facing = std::remainder(m_facing - m_input.x * t.turnSpeed * dt, 6.2831853f);
    const glm::vec2 forward(std::cos(m_facing), std::sin(m_facing));
    const glm::vec2 wish = forward * (m_input.y < 0.0f ? m_input.y * backSpeed : m_input.y);

    // Running: ease towards the wished-for speed.
    glm::vec2 run(velocity.x, velocity.y);
    const glm::vec2 target = wish * t.runSpeed;
    const float accel =
        (m_grounded ? t.groundAcceleration : t.airAcceleration) * dt;
    const glm::vec2 change = target - run;
    run +=
        glm::length(change) > accel ? glm::normalize(change) * accel : change;

    // Dashing: a burst along the way it's going (or facing), level.
    if (m_dashQueued && m_dashReady && m_dashClock <= 0.0f)
    {
      m_dashClock = t.dashTime;
      m_dashReady = m_grounded; // once in the air until it lands
      m_dashDirection = glm::length(wish) > 0.1f
                            ? glm::normalize(wish)
                            : glm::vec2(std::cos(m_facing), std::sin(m_facing));
      if (m_director)
        m_director->burst("dash", transform.position + glm::vec3(0, 0, 0.6f));
    }
    m_dashQueued = false;
    if (m_dashClock > 0.0f)
    {
      m_dashClock -= dt;
      run = m_dashDirection * t.dashSpeed;
      velocity.z = std::max(velocity.z, 0.0f);
    }

    // Jumping: from the ground (or just off it), then again in the air.
    m_jumpQueued = std::max(m_jumpQueued - dt, 0.0f);
    if (m_jumpQueued > 0.0f && (m_grounded || m_coyote > 0.0f))
    {
      velocity.z = t.jumpSpeed;
      m_jumpQueued = m_coyote = 0.0f;
      m_grounded = false;
      squash(1.25f);
      if (m_director)
        m_director->burst("dust", transform.position);
    }
    else if (m_jumpQueued > 0.0f && !m_grounded && m_airJumps > 0)
    {
      velocity.z = t.jumpSpeed * 0.92f;
      m_jumpQueued = 0.0f;
      --m_airJumps;
      squash(1.3f);
      if (m_director)
        m_director->burst("puff", transform.position + glm::vec3(0, 0, 0.3f));
    }
    // A short hop when jump is let go early; and a snappier fall.
    if (!m_jumpHeld && velocity.z > 0.0f)
      velocity.z *= std::max(1.0f - 9.0f * dt, 0.0f);
    if (velocity.z < 0.0f && m_dashClock <= 0.0f)
      velocity.z -= 7.0f * dt;

    m_physics->setVelocity(entity(), glm::vec3(run, velocity.z));

    // The model: turned the way it faces, squashed back into shape.
    m_stretch += (1.0f - m_stretch) * std::min(12.0f * dt, 1.0f);
    if (m_model.isValid() && m_model.hasComponent<sfs::TransformComponent>())
    {
      auto& model = m_model.getComponent<sfs::TransformComponent>();
      model.rotation = glm::angleAxis(m_facing, glm::vec3(0.0f, 0.0f, 1.0f));
      const float wide = 1.0f / std::sqrt(m_stretch);
      model.scale = glm::vec3(wide, wide, m_stretch);
    }
  }

  void onTriggerEnter(const sfs::Collision3D& collision) override
  {
    if (m_director && collision.other.isValid() &&
        collision.other.hasComponent<Spark>())
      m_director->collect(collision.other);
  }

private:
  const HeroTuning& settings() const
  {
    static const HeroTuning defaults;
    const HeroTuning* chosen = tuning.get();
    return chosen ? *chosen : defaults;
  }

  void land(const glm::vec3& at, float speed)
  {
    squash(std::max(0.65f, 1.0f - speed * 0.03f));
    if (m_director)
    {
      m_director->burst("dust", at);
      m_director->shake(std::min(speed * 0.02f, 0.25f));
    }
  }

  void squash(float stretch) { m_stretch = stretch; }

  void respawn()
  {
    auto& transform = entity().getComponent<sfs::TransformComponent>();
    transform.position = m_spawn + glm::vec3(0.0f, 0.0f, 0.5f);
    if (m_physics)
      m_physics->setVelocity(entity(), glm::vec3(0.0f));
    m_dashClock = 0.0f;
    if (m_director)
      m_director->burst("sparkle", transform.position + glm::vec3(0, 0, 0.8f));
  }

  sfs::PhysicsSystem3D* m_physics = nullptr;
  IslandDirector* m_director = nullptr;
  sfs::Entity m_model;
  glm::vec3 m_spawn{0.0f};
  glm::vec2 m_input{0.0f};
  bool m_jumpHeld = false;
  float m_jumpQueued = 0.0f;
  bool m_dashQueued = false;
  bool m_dashReady = true;
  float m_dashClock = 0.0f;
  glm::vec2 m_dashDirection{1.0f, 0.0f};
  bool m_grounded = false;
  float m_coyote = 0.0f;
  int m_airJumps = 0;
  float m_fallSpeed = 0.0f;
  float m_facing = 0.0f;
  float m_stretch = 1.0f;
};
