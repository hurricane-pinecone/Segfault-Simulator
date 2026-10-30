#pragma once
#include <sfs/sfs.h>

#include "components/Drone.h"
#include "components/Hazard.h"
#include "components/JumpPad.h"
#include "components/Orb.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <string>

class RunHud;

// Runs a Neon Sprint run: builds the course ahead of the runner out of
// platforms, orbs, spikes, drones and jump pads (harder the further it goes),
// clears it away behind, drives the glitch wall that chases the runner, keeps
// the score and combo, and owns the effects and the HUD.
//
// World units: the scene's Space2D draws 4 of them an art pixel, so a 16-pixel
// tile is 64 across. y runs down.
class RunDirector : public sfs::GameObject
{
public:
  // The art: the props sheet (16 x 16 frames: 0 block top, 1 block, 2 spikes,
  // 3-6 orb, 7-8 drone, 9 beacon, 10 jump pad) and the glitch wall's.
  [[sfs::texture]] std::string props;
  [[sfs::texture]] std::string glitch;

  // The glitch wall: how fast it starts, how quickly it speeds up (a second),
  // its top speed, and how far behind the runner it ever falls.
  float wallSpeed = 300.0f;
  float wallAcceleration = 7.0f;
  float wallTopSpeed = 720.0f;
  float wallMaxLag = 1500.0f;

  // How long a combo lasts without a new orb or drone (seconds).
  float comboTime = 2.2f;

  // Where the run's scene is, to start again.
  std::string sceneFile = "scenes/run.scene";

  static constexpr float kTile = 64.0f;

  // --- For the runner and the HUD.
  float timeScale() const { return m_hitStop > 0.0f ? 0.06f : 1.0f; }
  float wallX() const { return m_wallX; }
  bool over() const { return m_over; }
  int score() const { return m_score; }
  int best() const { return s_best; }
  int combo() const { return m_combo; }
  float comboLeft() const
  {
    return m_combo > 1 ? m_comboClock / comboTime : 0.0f;
  }
  int distance() const
  {
    return static_cast<int>(std::max(m_furthest, 0.0f) / kTile);
  }
  float clock() const { return m_clock; }
  float fallLine() const { return m_lowest + 900.0f; }

  void collect(const sfs::Entity& orb)
  {
    const glm::vec2 at =
        orb.getComponent<sfs::TransformComponent>().position2D();
    bump(orb.getComponent<Orb>().value);
    particles().spawnBurst("spark", at);
    scene().destroyEntity(orb);
  }

  void destroyDrone(const sfs::Entity& drone)
  {
    const glm::vec2 at =
        drone.getComponent<sfs::TransformComponent>().position2D();
    bump(50);
    particles().spawnBurst("boom", at);
    shake(0.55f, 0.25f);
    m_hitStop = 0.07f;
    scene().destroyEntity(drone);
  }

  void bounce(glm::vec2 at)
  {
    particles().spawnBurst("ring", at);
    shake(0.2f, 0.12f);
  }

  void puff(const std::string& effect, glm::vec2 at)
  {
    particles().spawnBurst(effect, at);
  }

  void shake(float strength, float seconds)
  {
    if (m_camera.isValid() &&
        m_camera.hasComponent<sfs::CameraFollowComponent>())
      m_camera.getComponent<sfs::CameraFollowComponent>().shake(
          strength, seconds);
  }

  // The runner hit something (or fell, or the wall caught it): the run ends.
  void end(glm::vec2 at)
  {
    if (m_over)
      return;
    m_over = true;
    m_overClock = 0.0f;
    s_best = std::max(s_best, m_score);
    particles().spawnBurst("shatter", at);
    particles().spawnBurst("boom", at);
    shake(1.0f, 0.6f);
    m_hitStop = 0.12f;
  }

  void restart() { scene().game().loadScene(sceneFile); }

protected:
  void onCreate(sfs::Scene& scene) override;

  void onUpdate(double deltaTime) override
  {
    const float dt = static_cast<float>(deltaTime);
    m_hitStop = std::max(m_hitStop - dt, 0.0f);
    const float step = dt * timeScale();
    m_clock += step;

    const glm::vec2 runner = runnerAt();
    m_furthest = std::max(m_furthest, runner.x);

    // The combo runs out.
    if (m_combo > 0 && (m_comboClock -= step) <= 0.0f)
      m_combo = 0;

    // The wall comes on (once the runner sets off), faster and faster, never
    // far behind.
    if (!m_started)
      m_started = runner.x > m_startX + kTile * 0.5f || m_clock > 4.0f;
    if (m_over)
    {
      if ((m_overClock += dt) > 1.4f)
        restart();
    }
    else if (m_started)
    {
      m_wallSpeed =
          std::min(m_wallSpeed + wallAcceleration * step, wallTopSpeed);
      m_wallX = std::max(m_wallX + m_wallSpeed * step, runner.x - wallMaxLag);
      if (runner.x - 20.0f < m_wallX)
        end(runner);
    }
    placeWall(runner);

    animate(step);
    build(runner.x + 2800.0f);
    clearBehind(m_wallX - 900.0f);
  }

  void onProcessInput(const sfs::Input& input) override
  {
    if (input.keyboard().keyPressed(sfs::Key::R))
      restart();
  }

private:
  sfs::ParticleModule& particles()
  {
    return *scene()
                .getSystem<sfs::RenderSystem>()
                .module<sfs::ParticleModule>();
  }

  glm::vec2 runnerAt() const
  {
    return m_runner.isValid()
               ? m_runner.getComponent<sfs::TransformComponent>().position2D()
               : glm::vec2(0.0f);
  }

  void bump(int points)
  {
    m_combo = std::min(m_combo + 1, 9);
    m_comboClock = comboTime;
    m_score += points * std::max(m_combo, 1);
  }

  // --- Building the course.

  sfs::Entity sprite(glm::vec2 at,
                     int frame,
                     int layer,
                     glm::vec2 scale = glm::vec2(1.0f),
                     const std::string* image = nullptr)
  {
    sfs::Entity entity = scene().createEntity();
    entity.addComponent<sfs::TransformComponent>(
        sfs::TransformComponent(at, scale));
    sfs::SpriteComponent look;
    look.image = image ? *image : props;
    look.frame = frame;
    entity.addComponent<sfs::SpriteComponent>(look);
    entity.addComponent<sfs::RenderLayerComponent>(layer);
    m_spawned.push_back({at.x + scale.x * kTile, entity});
    return entity;
  }

  // A run of blocks `tiles` wide whose top is at `top`, `depth` blocks deep,
  // solid (with a collider particles stick to and the runner stands on).
  void platform(float left, float top, int tiles, int depth)
  {
    for (int column = 0; column < tiles; ++column)
      for (int row = 0; row < depth; ++row)
        sprite({left + (column + 0.5f) * kTile, top + (row + 0.5f) * kTile},
               row == 0 ? 0 : 1,
               0);
    sfs::Entity solid = scene().createEntity();
    const glm::vec2 half(tiles * kTile * 0.5f, depth * kTile * 0.5f);
    solid.addComponent<sfs::TransformComponent>(
        sfs::TransformComponent(glm::vec2(left, top) + half));
    solid.addComponent<sfs::BoxCollider2D>(half);
    solid.addComponent<sfs::SolidObject>();
    m_spawned.push_back({left + tiles * kTile, solid});
    m_lowest = std::max(m_lowest, top + depth * kTile);
  }

  void orb(glm::vec2 at, bool lit)
  {
    sfs::Entity entity = sprite(at, 3, 1, glm::vec2(0.75f));
    entity.addComponent<Orb>();
    if (lit)
      entity.addComponent<sfs::PointLightComponent>(
          glm::vec3(1.0f, 0.8f, 0.3f),
          1.4f,
          220.0f,
          glm::vec3(0.0f, 0.0f, 40.0f));
  }

  void spikes(float left, float top, int tiles)
  {
    for (int i = 0; i < tiles; ++i)
    {
      sfs::Entity entity =
          sprite({left + (i + 0.5f) * kTile, top - kTile * 0.5f}, 2, 1);
      Hazard hurt;
      hurt.half = {kTile * 0.42f, kTile * 0.2f};
      hurt.offset = {0.0f, kTile * 0.2f};
      entity.addComponent<Hazard>(hurt);
    }
  }

  void drone(glm::vec2 at)
  {
    sfs::Entity entity = sprite(at, 7, 2);
    Drone hover;
    hover.home = at;
    hover.clock = m_random() % 100 * 0.06f;
    entity.addComponent<Drone>(hover);
    entity.addComponent<sfs::PointLightComponent>(glm::vec3(1.0f, 0.2f, 0.7f),
                                                  1.2f,
                                                  200.0f,
                                                  glm::vec3(0.0f, 0.0f, 40.0f));
  }

  void beacon(glm::vec2 groundAt)
  {
    sfs::Entity entity =
        sprite({groundAt.x, groundAt.y - kTile}, 9, -1, glm::vec2(1.0f, 2.0f));
    entity.addComponent<sfs::PointLightComponent>(glm::vec3(1.0f, 0.85f, 0.4f),
                                                  2.2f,
                                                  420.0f,
                                                  glm::vec3(0.0f, 0.0f, 60.0f));
    entity.getComponent<sfs::TransformComponent>().position.y -= kTile * 0.5f;
  }

  void jumpPad(glm::vec2 groundAt)
  {
    sfs::Entity entity = sprite({groundAt.x, groundAt.y - kTile * 0.5f}, 10, 1);
    entity.addComponent<JumpPad>();
  }

  int roll(int low, int high)
  {
    return std::uniform_int_distribution<int>(low, high)(m_random);
  }
  bool chance(float p)
  {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_random) < p;
  }

  // An arc of orbs from `from` to `to`, rising `lift` in the middle.
  void orbArc(glm::vec2 from, glm::vec2 to, float lift, int count)
  {
    for (int i = 0; i < count; ++i)
    {
      const float t = count == 1 ? 0.5f : static_cast<float>(i) / (count - 1);
      glm::vec2 at = glm::mix(from, to, t);
      at.y -= lift * 4.0f * t * (1.0f - t);
      orb(at, i == count / 2);
    }
  }

  void build(float until)
  {
    while (m_cursor < until)
    {
      // Harder as it goes: wider gaps, more spikes and drones.
      const float hard = std::clamp(m_cursor / 24000.0f, 0.0f, 1.0f);
      const int gap = roll(1, 2 + static_cast<int>(hard * 2.5f));
      const float left = m_cursor + gap * kTile;
      const float top =
          std::clamp(m_top + roll(-2, 2) * kTile, -5.0f * kTile, 2.0f * kTile);
      const int kind = roll(0, 9);

      // Over the gap: orbs to follow, sometimes a drone in the way.
      orbArc({m_cursor + kTile * 0.5f, m_top - kTile},
             {left - kTile * 0.5f, top - kTile},
             kTile * (1.5f + gap * 0.3f),
             gap + 1);
      if (gap >= 3 && chance(0.25f + hard * 0.5f))
        drone({(m_cursor + left) * 0.5f, std::min(m_top, top) - kTile * 2.5f});

      if (kind <= 4) // a run of ground, maybe spiked
      {
        const int tiles = roll(4, 9);
        platform(left, top, tiles, 3);
        if (tiles >= 6 && chance(0.3f + hard * 0.5f))
          spikes(left + roll(2, tiles - 3) * kTile, top, roll(1, 2));
        if (chance(0.35f))
          beacon({left + kTile * 1.5f, top});
        m_cursor = left + tiles * kTile;
        m_top = top;
      }
      else if (kind <= 6) // stepping stones up into the air
      {
        float x = left;
        float y = top;
        for (int step = 0; step < 3; ++step)
        {
          platform(x, y, 2, 1);
          orb({x + kTile, y - kTile * 1.2f}, false);
          x += kTile * (2 + roll(2, 3));
          y -= kTile * roll(0, 2);
        }
        m_cursor = x - kTile * 2;
        m_top = y + kTile;
      }
      else if (kind == 7) // a pad, and a long flight
      {
        platform(left, top, 4, 3);
        jumpPad({left + kTile * 2.5f, top});
        const float landing = left + kTile * (4 + roll(5, 7));
        const float high = std::max(top - kTile * 4, -6.0f * kTile);
        orbArc({left + kTile * 3, top - kTile * 2},
               {landing, high - kTile},
               kTile * 3.0f,
               6);
        platform(landing, high, 5, 2);
        m_cursor = landing + kTile * 5;
        m_top = high;
      }
      else // a ledge with a drone patrolling above it
      {
        const int tiles = roll(6, 8);
        platform(left, top, tiles, 3);
        drone({left + tiles * kTile * 0.5f, top - kTile * 2.2f});
        m_cursor = left + tiles * kTile;
        m_top = top;
      }
    }
  }

  void clearBehind(float x)
  {
    while (!m_spawned.empty() && m_spawned.front().first < x)
    {
      if (m_spawned.front().second.isValid())
        scene().destroyEntity(m_spawned.front().second);
      m_spawned.pop_front();
    }
  }

  // --- Moving things.

  void animate(float step)
  {
    for (sfs::Entity& entity : scene().view<Orb, sfs::SpriteComponent>())
    {
      Orb& orb = entity.getComponent<Orb>();
      orb.spin += step * 10.0f;
      entity.getComponent<sfs::SpriteComponent>().frame =
          3 + static_cast<int>(orb.spin) % 4;
    }
    for (sfs::Entity& entity : scene().view<Drone, sfs::TransformComponent>())
    {
      Drone& drone = entity.getComponent<Drone>();
      drone.clock += step;
      entity.getComponent<sfs::TransformComponent>().setPosition2D(
          drone.home +
          glm::vec2(
              std::sin(drone.clock * drone.speed * 0.5f) * drone.amplitude,
              std::sin(drone.clock * drone.speed) * drone.amplitude * 0.5f));
      if (entity.hasComponent<sfs::SpriteComponent>())
        entity.getComponent<sfs::SpriteComponent>().frame =
            7 + static_cast<int>(drone.clock * 12.0f) % 2;
    }
  }

  void placeWall(glm::vec2 runner)
  {
    if (!m_wall.isValid())
      return;
    auto& transform = m_wall.getComponent<sfs::TransformComponent>();
    transform.setPosition2D({m_wallX - kTile, runner.y - kTile * 2.0f});
    m_wall.getComponent<sfs::SpriteComponent>().frame =
        static_cast<int>(m_clock * 16.0f) % 2;
  }

  void registerEffects();

  sfs::Entity m_runner;
  sfs::Entity m_camera;
  sfs::Entity m_wall;
  std::mt19937 m_random{std::random_device{}()};
  std::deque<std::pair<float, sfs::Entity>> m_spawned; // by where they end
  float m_cursor = 0.0f; // the course is built up to here
  float m_top = 0.0f;    // the last ground's top
  float m_lowest = 0.0f; // the lowest ground's bottom
  float m_furthest = 0.0f;
  float m_wallX = -1400.0f;
  float m_wallSpeed = 0.0f;
  float m_clock = 0.0f;
  float m_hitStop = 0.0f;
  float m_comboClock = 0.0f;
  float m_overClock = 0.0f;
  int m_score = 0;
  int m_combo = 0;
  bool m_over = false;
  bool m_started = false; // the runner has set off
  float m_startX = 0.0f;
  static inline int s_best = 0; // across restarts, while the game runs
};

#include "RunHud.h"

inline void RunDirector::onCreate(sfs::Scene& scene)
{
  m_runner = scene.findEntity("Runner");
  m_startX = runnerAt().x;
  m_camera = scene.findEntity("Camera");
  m_wallSpeed = wallSpeed;
  registerEffects();

  // The ground the run starts on (the scene's own), and where the course
  // carries on from.
  if (const sfs::Entity start = scene.findEntity("Start Ground");
      start.isValid())
  {
    const auto& transform = start.getComponent<sfs::TransformComponent>();
    const auto& box = start.getComponent<sfs::BoxCollider2D>();
    m_cursor = transform.position.x + box.half.x;
    m_top = transform.position.y - box.half.y;
    m_lowest = transform.position.y + box.half.y;
  }

  // The wall: a column of noise down the screen, lit magenta.
  m_wall = scene.createEntity();
  m_wall.addComponent<sfs::TransformComponent>(
      sfs::TransformComponent(glm::vec2(m_wallX, 0.0f), glm::vec2(2.5f, 5.0f)));
  sfs::SpriteComponent look;
  look.image = glitch;
  look.tint = glm::vec4(1.4f, 1.2f, 1.4f, 0.92f);
  m_wall.addComponent<sfs::SpriteComponent>(look);
  m_wall.addComponent<sfs::RenderLayerComponent>(4);
  m_wall.addComponent<sfs::PointLightComponent>(glm::vec3(1.0f, 0.25f, 0.8f),
                                                4.0f,
                                                900.0f,
                                                glm::vec3(0.0f, 0.0f, 120.0f));

  scene.createObject<RunHud>().setDirector(this);
}

inline void RunDirector::registerEffects()
{
  sfs::ParticleModule& fx = particles();
  fx.setMaxParticles(6000);

  sfs::ParticleEffectDesc dust; // feet on the ground
  dust.burstCount = 10;
  dust.shape = sfs::EmissionShape::Box;
  dust.boxExtents = {18.0f, 2.0f};
  dust.lifetime = {0.25f, 0.5f};
  dust.speed = {40.0f, 160.0f};
  dust.directionSpread = 3.1f;
  dust.size = {8.0f, 18.0f};
  dust.gravity = {0.0f, -120.0f};
  dust.drag = 3.0f;
  dust.sizeOverLife = sfs::Curve::linear(1.0f, 0.3f);
  dust.alphaOverLife = sfs::Curve::linear(0.7f, 0.0f);
  dust.colorOverLife =
      sfs::Gradient::twoStop({0.6f, 0.65f, 0.9f}, {0.3f, 0.3f, 0.5f});
  fx.registerEffect("dust", dust);

  sfs::ParticleEffectDesc trail = dust; // the dash's afterglow
  trail.burstCount = 18;
  trail.boxExtents = {12.0f, 22.0f};
  trail.speed = {10.0f, 60.0f};
  trail.size = {10.0f, 22.0f};
  trail.gravity = {0.0f, 0.0f};
  trail.colorOverLife =
      sfs::Gradient::twoStop({0.3f, 1.0f, 1.0f}, {0.2f, 0.3f, 1.0f});
  trail.blend = sfs::BlendMode::Additive;
  fx.registerEffect("trail", trail);

  sfs::ParticleEffectDesc spark; // an orb taken
  spark.burstCount = 24;
  spark.lifetime = {0.3f, 0.7f};
  spark.speed = {120.0f, 480.0f};
  spark.size = {6.0f, 14.0f};
  spark.gravity = {0.0f, 600.0f};
  spark.drag = 2.0f;
  spark.sizeOverLife = sfs::Curve::linear(1.0f, 0.2f);
  spark.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  spark.colorOverLife =
      sfs::Gradient::twoStop({1.0f, 0.95f, 0.5f}, {1.0f, 0.45f, 0.1f});
  spark.blend = sfs::BlendMode::Additive;
  fx.registerEffect("spark", spark);

  sfs::ParticleEffectDesc ring = spark; // a jump in the air, a bounce
  ring.burstCount = 16;
  ring.shape = sfs::EmissionShape::Ring;
  ring.shapeRadius = 14.0f;
  ring.speed = {160.0f, 220.0f};
  ring.gravity = {0.0f, 0.0f};
  ring.colorOverLife =
      sfs::Gradient::twoStop({0.9f, 0.95f, 1.0f}, {0.3f, 0.9f, 1.0f});
  fx.registerEffect("ring", ring);

  sfs::ParticleEffectDesc boom; // a drone destroyed; its oil marks the ground
  boom.burstCount = 60;
  boom.lifetime = {0.4f, 1.1f};
  boom.speed = {200.0f, 760.0f};
  boom.size = {8.0f, 24.0f};
  boom.gravity = {0.0f, 1500.0f};
  boom.drag = 0.6f;
  boom.sizeOverLife = sfs::Curve::linear(1.0f, 0.4f);
  boom.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  boom.colorOverLife =
      sfs::Gradient::twoStop({1.0f, 0.3f, 0.85f}, {0.5f, 0.1f, 0.6f});
  boom.ground = sfs::GroundBehavior::Die;
  boom.leavesDecal = true;
  boom.decal.pool.size = {6.0f, 18.0f};
  boom.decal.pool.soft = true;
  boom.decal.useParticleColor = true;
  boom.decal.pool.alpha = 0.8f;
  boom.decal.impactRef = 600.0f;
  fx.registerEffect("boom", boom);

  sfs::ParticleEffectDesc shatter = boom; // the runner, in pieces
  shatter.burstCount = 120;
  shatter.speed = {300.0f, 1100.0f};
  shatter.colorOverLife =
      sfs::Gradient::twoStop({0.4f, 1.0f, 1.0f}, {1.0f, 0.3f, 0.8f});
  shatter.look = sfs::ParticleShape::Pixel;
  fx.registerEffect("shatter", shatter);
}
