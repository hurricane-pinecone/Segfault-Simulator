#pragma once
#include <sfs/sfs.h>

#include "RunDirector.h"

#include <algorithm>
#include <cmath>

// The runner: run, jump (again in the air), dash, slide down walls and jump off
// them. Forgiving where it counts: a jump pressed just before landing still
// happens (buffering), and one pressed just after running off a ledge too
// (coyote time); letting go of jump early makes a shorter hop.
//
// Its entity's transform is at its feet. It collides with the scene's solids
// (SolidObject + BoxCollider2D), takes orbs, breaks drones it dashes through or
// lands on, and ends the run on spikes, a drone's side, a fall or the wall.
class Runner : public sfs::GameObject
{
public:
  // Running (world units a second, and a second squared).
  float runSpeed = 620.0f;
  float groundAcceleration = 6400.0f;
  float airAcceleration = 4200.0f;
  // Jumping.
  float jumpSpeed = 1200.0f;
  float airJumpSpeed = 1040.0f;
  float gravity = 3600.0f;
  float fallGravityScale = 1.75f; // falling, or jump let go: heavier
  float maxFallSpeed = 1600.0f;
  float coyoteTime = 0.1f;
  float jumpBuffer = 0.12f;
  int airJumps = 1;
  // Dashing.
  float dashSpeed = 1650.0f;
  float dashTime = 0.15f;
  // Walls.
  float wallSlideSpeed = 300.0f;
  glm::vec2 wallJump{700.0f, 1080.0f};
  // The body's box, about its feet (half its width, its height).
  glm::vec2 size{18.0f, 54.0f};

protected:
  void onCreate(sfs::Scene& scene) override
  {
    m_director = scene.tryFindObject<RunDirector>();
    // The camera follows, looking a little ahead.
    if (sfs::Entity camera = scene.findEntity("Camera");
        camera.isValid() && camera.hasComponent<sfs::CameraFollowComponent>())
      camera.getComponent<sfs::CameraFollowComponent>().target =
          static_cast<int>(entity().getId());
  }

  void onProcessInput(const sfs::Input& input) override
  {
    const sfs::KeyboardInput& keys = input.keyboard();
    m_move =
        (keys.keyHeld(sfs::Key::D) || keys.keyHeld(sfs::Key::Right) ? 1.0f
                                                                    : 0.0f) -
        (keys.keyHeld(sfs::Key::A) || keys.keyHeld(sfs::Key::Left) ? 1.0f
                                                                   : 0.0f);
    const bool jump = keys.keyHeld(sfs::Key::Space) ||
                      keys.keyHeld(sfs::Key::W) || keys.keyHeld(sfs::Key::Up) ||
                      keys.keyHeld(sfs::Key::K);
    if (jump && !m_jumpHeld)
      m_jumpQueued = jumpBuffer;
    m_jumpHeld = jump;
    if (keys.keyPressed(sfs::Key::LeftShift) ||
        keys.keyPressed(sfs::Key::RightShift) || keys.keyPressed(sfs::Key::J) ||
        keys.keyPressed(sfs::Key::X))
      m_dashQueued = true;
  }

  void onUpdate(double deltaTime) override
  {
    if (!m_director || m_director->over())
    {
      if (m_director && m_director->over() &&
          entity().hasComponent<sfs::SpriteComponent>())
        entity().removeComponent<sfs::SpriteComponent>(); // in pieces
      return;
    }
    const float dt = std::min(static_cast<float>(deltaTime), 1.0f / 30.0f) *
                     m_director->timeScale();
    auto& transform = entity().getComponent<sfs::TransformComponent>();
    glm::vec2 at = transform.position2D();

    m_coyote -= dt;
    m_jumpQueued -= dt;
    if (m_grounded)
    {
      m_coyote = coyoteTime;
      m_airJumpsLeft = airJumps;
      m_canDash = true;
    }

    // Dashing: straight and fast, no gravity, where it's facing.
    if (m_dashQueued && m_canDash && m_dashClock <= 0.0f)
    {
      m_dashClock = dashTime;
      m_canDash = false;
      m_velocity = {m_facing * dashSpeed, 0.0f};
      m_director->shake(0.2f, 0.1f);
    }
    m_dashQueued = false;

    if (m_dashClock > 0.0f)
    {
      m_dashClock -= dt;
      m_trailClock -= dt;
      if (m_trailClock <= 0.0f)
      {
        m_director->puff("trail", at - glm::vec2(0.0f, size.y * 0.5f));
        m_trailClock = 0.03f;
      }
      if (m_dashClock <= 0.0f)
        m_velocity.x = m_facing * runSpeed;
    }
    else
    {
      // Running: quick to speed up and to stop.
      const float target = m_move * runSpeed;
      const float accel = m_grounded ? groundAcceleration : airAcceleration;
      m_velocity.x +=
          std::clamp(target - m_velocity.x, -accel * dt, accel * dt);
      if (m_move != 0.0f)
        m_facing = m_move;

      // Jumping: from the ground (or just off it), off a wall, or in the air.
      if (m_jumpQueued > 0.0f)
      {
        if (m_grounded || m_coyote > 0.0f)
          jump(jumpSpeed, at);
        else if (m_wall != 0)
        {
          m_velocity = {-m_wall * wallJump.x, -wallJump.y};
          m_facing = static_cast<float>(-m_wall);
          m_jumpQueued = 0.0f;
          m_director->puff(
              "dust", at + glm::vec2(m_wall * size.x, -size.y * 0.5f));
        }
        else if (m_airJumpsLeft > 0)
        {
          --m_airJumpsLeft;
          jump(airJumpSpeed, at);
          m_director->puff("ring", at);
        }
      }

      // Falling, faster on the way down and on a short hop.
      const bool rising = m_velocity.y < 0.0f;
      const float pull =
          gravity * (rising && m_jumpHeld ? 1.0f : fallGravityScale);
      m_velocity.y = std::min(m_velocity.y + pull * dt, maxFallSpeed);
      // Sliding down a wall it's pushing into.
      if (m_wall != 0 && m_move == static_cast<float>(m_wall) &&
          m_velocity.y > wallSlideSpeed)
        m_velocity.y = wallSlideSpeed;
    }

    const bool wasGrounded = m_grounded;
    const float fallSpeed = m_velocity.y;
    at = moveAndCollide(at, dt);
    transform.setPosition2D(at);
    if (m_grounded && !wasGrounded && fallSpeed > 900.0f)
    {
      m_director->puff("dust", at);
      m_director->shake(std::min(fallSpeed / 6000.0f, 0.3f), 0.1f);
    }

    touch(at);
    if (at.y > m_director->fallLine())
      m_director->end(at);
    animate(dt);
  }

private:
  void jump(float speed, glm::vec2 at)
  {
    m_velocity.y = -speed;
    m_jumpQueued = 0.0f;
    m_coyote = 0.0f;
    m_grounded = false;
    m_director->puff("dust", at);
  }

  struct Box
  {
    glm::vec2 min;
    glm::vec2 max;
  };

  Box box(glm::vec2 at) const
  {
    return {at - glm::vec2(size.x, size.y), at + glm::vec2(size.x, 0.0f)};
  }

  static bool overlaps(const Box& a, const Box& b)
  {
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y &&
           a.max.y > b.min.y;
  }

  // Move along x, then y, stopping at solids; note the ground and walls.
  glm::vec2 moveAndCollide(glm::vec2 at, float dt)
  {
    std::vector<Box> solids;
    const Box near{at - glm::vec2(900.0f), at + glm::vec2(900.0f)};
    for (sfs::Entity& solid : scene()
                                  .view<sfs::SolidObject,
                                        sfs::BoxCollider2D,
                                        sfs::TransformComponent>())
    {
      const auto& collider = solid.getComponent<sfs::BoxCollider2D>();
      const glm::vec2 centre =
          solid.getComponent<sfs::TransformComponent>().position2D() +
          collider.offset;
      const Box b{centre - collider.half, centre + collider.half};
      if (overlaps(b, near))
        solids.push_back(b);
    }

    at.x += m_velocity.x * dt;
    m_wall = 0;
    for (const Box& solid : solids)
      if (overlaps(box(at), solid))
      {
        if (m_velocity.x > 0.0f)
          at.x = solid.min.x - size.x;
        else if (m_velocity.x < 0.0f)
          at.x = solid.max.x + size.x;
        if (m_dashClock > 0.0f)
          m_dashClock = 0.0f;
        m_velocity.x = 0.0f;
      }
    // Touching a wall at either side (for sliding and wall jumps).
    for (const Box& solid : solids)
    {
      const Box body = box(at);
      if (body.max.y - 8.0f > solid.min.y && body.min.y + 8.0f < solid.max.y)
      {
        if (std::abs(body.max.x - solid.min.x) < 2.0f)
          m_wall = 1;
        else if (std::abs(body.min.x - solid.max.x) < 2.0f)
          m_wall = -1;
      }
    }

    at.y += m_velocity.y * dt;
    m_grounded = false;
    for (const Box& solid : solids)
      if (overlaps(box(at), solid))
      {
        if (m_velocity.y >= 0.0f)
        {
          at.y = solid.min.y;
          m_grounded = true;
        }
        else
          at.y = solid.max.y + size.y;
        m_velocity.y = 0.0f;
      }
    if (m_grounded)
      m_wall = 0;
    return at;
  }

  // Orbs, spikes, drones and pads it's touching.
  void touch(glm::vec2 at)
  {
    const Box body = box(at);
    for (sfs::Entity& orb : scene().view<Orb, sfs::TransformComponent>())
      if (glm::distance(
              orb.getComponent<sfs::TransformComponent>().position2D(),
              at - glm::vec2(0.0f, size.y * 0.5f)) < 58.0f)
        m_director->collect(orb);

    for (sfs::Entity& spike : scene().view<Hazard, sfs::TransformComponent>())
    {
      const Hazard& hurt = spike.getComponent<Hazard>();
      const glm::vec2 centre =
          spike.getComponent<sfs::TransformComponent>().position2D() +
          hurt.offset;
      if (overlaps(body, {centre - hurt.half, centre + hurt.half}))
      {
        m_director->end(at);
        return;
      }
    }

    for (sfs::Entity& drone : scene().view<Drone, sfs::TransformComponent>())
    {
      const glm::vec2 centre =
          drone.getComponent<sfs::TransformComponent>().position2D();
      if (!overlaps(body,
                    {centre - glm::vec2(30.0f, 22.0f),
                     centre + glm::vec2(30.0f, 22.0f)}))
        continue;
      if (m_dashClock > 0.0f)
        m_director->destroyDrone(drone);
      else if (m_velocity.y > 0.0f && at.y < centre.y)
      {
        // Stomped: a bounce, and a jump back in the air.
        m_director->destroyDrone(drone);
        m_velocity.y = -jumpSpeed * 0.85f;
        m_airJumpsLeft = airJumps;
        m_canDash = true;
      }
      else
      {
        m_director->end(at);
        return;
      }
    }

    if (m_grounded)
      for (sfs::Entity& pad : scene().view<JumpPad, sfs::TransformComponent>())
      {
        const glm::vec2 centre =
            pad.getComponent<sfs::TransformComponent>().position2D();
        if (std::abs(centre.x - at.x) < 40.0f &&
            std::abs(centre.y + 32.0f - at.y) < 40.0f)
        {
          m_velocity.y = -pad.getComponent<JumpPad>().launch;
          m_grounded = false;
          m_airJumpsLeft = airJumps;
          m_canDash = true;
          m_director->bounce(at);
        }
      }
  }

  // The frame for what it's doing: 0-1 idle, 2-5 run, 6 jump, 7 fall, 8 dash,
  // 9 wall.
  void animate(float dt)
  {
    if (!entity().hasComponent<sfs::SpriteComponent>())
      return;
    m_animClock += dt;
    int frame = 0;
    if (m_dashClock > 0.0f)
      frame = 8;
    else if (!m_grounded && m_wall != 0)
      frame = 9;
    else if (!m_grounded)
      frame = m_velocity.y < 0.0f ? 6 : 7;
    else if (std::abs(m_velocity.x) > 40.0f)
      frame = 2 + static_cast<int>(m_animClock * 14.0f) % 4;
    else
      frame = static_cast<int>(m_animClock * 2.5f) % 2;
    entity().getComponent<sfs::SpriteComponent>().frame = frame;
    // Facing: the sprite turns round (scale x).
    auto& transform = entity().getComponent<sfs::TransformComponent>();
    transform.scale.x =
        std::abs(transform.scale.x) * (m_facing < 0.0f ? -1.0f : 1.0f);
  }

  RunDirector* m_director = nullptr;
  glm::vec2 m_velocity{0.0f};
  float m_move = 0.0f;
  float m_facing = 1.0f;
  bool m_jumpHeld = false;
  float m_jumpQueued = 0.0f;
  bool m_dashQueued = false;
  bool m_grounded = false;
  float m_coyote = 0.0f;
  int m_airJumpsLeft = 0;
  bool m_canDash = true;
  float m_dashClock = 0.0f;
  float m_trailClock = 0.0f;
  int m_wall = 0; // -1 a wall at its left, 1 at its right
  float m_animClock = 0.0f;
};
