#pragma once
#include <sfs/sfs.h>

#include "FollowCamera.h"
#include "components/Bob.h"
#include "components/Lantern.h"
#include "components/Pin.h"
#include "components/Spark.h"
#include "components/Station.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

class IslandHud;

// Glowfall Isle's rules: find every spark on the island. It keeps the count,
// runs the day (the sun rises and sets, lanterns and fireflies come out at
// night), drops a spark when the pins are bowled over, and celebrates at the
// end. It also holds the game's particle effects, and says what the station
// the hero is near shows.
class IslandDirector : public sfs::GameObject
{
public:
  // A whole day, in seconds (T held: much faster).
  [[sfs::range(20, 1200)]] float dayLength = 240.0f;
  // Where the day starts: 0 midnight, 0.25 dawn, 0.5 noon, 0.75 dusk.
  [[sfs::range(0, 1)]] float startTime = 0.36f;
  // How many pins must be down for the strike spark.
  [[sfs::range(1, 6)]] int pinsForStrike = 4;

  int collected() const { return m_collected; }
  int total() const { return m_total; }
  bool won() const { return m_total > 0 && m_collected >= m_total; }
  float clock() const { return m_clock; }
  /** The time of day, 0 to 1 (0.5 noon). */
  float timeOfDay() const { return m_time; }
  /** How dark it is, 0 (day) to 1 (night). */
  float darkness() const { return m_darkness; }
  /** A message to show for a moment (empty: none), and how much is left. */
  const std::string& message() const { return m_message; }
  float messageLeft() const { return m_messageClock; }
  /** The station the hero is near; null if none. */
  const Station* nearStation() const { return m_near; }

  /** Fire one of the game's particle effects at a point. */
  void burst(const std::string& effect, const glm::vec3& at)
  {
    if (sfs::ParticleModule* fx = particles())
      fx->spawnBurst(effect, glm::vec2(at), at.z);
  }

  void shake(float amount)
  {
    if (m_camera)
      m_camera->shake(amount);
  }

  /** The hero touched a spark: take it. */
  void collect(sfs::Entity spark)
  {
    if (!spark.isValid() || !spark.hasComponent<Spark>())
      return;
    const glm::vec3 at = sfs::hierarchy::worldPosition(spark);
    m_collected += spark.getComponent<Spark>().value;
    spark.removeComponent<Spark>(); // once
    scene().destroyBranch(spark);
    burst("sparkle", at);
    shake(0.18f);
    if (won())
    {
      say("Every spark found! The island glows.", 8.0f);
      m_celebrate = 6.0f;
    }
    else
      say(std::to_string(m_total - m_collected) + " to go", 1.6f);
  }

protected:
  void onCreate(sfs::Scene& scene) override;

  void onProcessInput(const sfs::Input& input) override
  {
    m_fastTime = input.keyboard().keyHeld(sfs::Key::T);
  }

  void onUpdate(double deltaTime) override
  {
    const float dt = static_cast<float>(deltaTime);
    m_clock += dt;
    m_messageClock = std::max(m_messageClock - dt, 0.0f);
    m_time =
        std::fmod(m_time + dt * (m_fastTime ? 12.0f : 1.0f) / dayLength, 1.0f);
    lightTheDay();
    checkPins();
    findNearStation();

    // The finale: fireworks over the island for a few seconds.
    if (m_celebrate > 0.0f)
    {
      m_celebrate -= dt;
      m_fireworkClock -= dt;
      if (m_fireworkClock <= 0.0f)
      {
        m_fireworkClock = 0.35f;
        const float a = m_clock * 2.3f;
        burst("firework",
              glm::vec3(std::cos(a) * 9.0f,
                        std::sin(a) * 9.0f,
                        9.0f + std::sin(a * 3.0f)));
      }
    }
  }

private:
  sfs::ParticleModule* particles()
  {
    auto* render = scene().tryGetSystem<sfs::RenderSystem>();
    return render ? render->module<sfs::ParticleModule>() : nullptr;
  }

  void say(std::string text, float seconds)
  {
    m_message = std::move(text);
    m_messageClock = seconds;
  }

  // The sun goes round; the sky, the light and the lamps follow it.
  void lightTheDay()
  {
    auto* render = scene().tryGetSystem<sfs::RenderSystem>();
    if (!render)
      return;
    const float angle = (m_time - 0.25f) * 6.2831853f; // 0 at dawn
    const float height = std::sin(angle);              // above the horizon
    const glm::vec3 sun = glm::normalize(
        glm::vec3(std::cos(angle), 0.45f, std::max(height, -0.3f)));
    const float day = glm::smoothstep(-0.12f, 0.25f, height);
    m_darkness = 1.0f - day;
    // Low sun: warm; high: white; night: a cool moonlight.
    const float low = 1.0f - glm::smoothstep(0.0f, 0.45f, std::abs(height));
    sfs::SceneLighting lighting = render->lighting();
    lighting.sun.direction =
        height > -0.05f ? sun : glm::normalize(glm::vec3(-0.3f, 0.6f, 0.75f));
    lighting.sun.color = height > -0.05f
                             ? glm::mix(glm::vec3(1.0f, 0.96f, 0.88f),
                                        glm::vec3(1.0f, 0.62f, 0.35f),
                                        low)
                             : glm::vec3(0.55f, 0.65f, 1.0f);
    lighting.sun.diffuseStrength = height > -0.05f ? 0.85f * day + 0.1f : 0.22f;
    lighting.ambient = glm::mix(0.16f, 0.45f, day);
    const glm::vec3 noon(0.45f, 0.66f, 0.92f), dusk(0.95f, 0.55f, 0.4f),
        night(0.02f, 0.03f, 0.09f);
    lighting.skyColor = glm::mix(night, glm::mix(noon, dusk, low), day);
    render->setLighting(lighting);

    for (sfs::Entity& lamp : scene().view<Lantern, sfs::PointLightComponent>())
    {
      const Lantern& lantern = lamp.getComponent<Lantern>();
      lamp.getComponent<sfs::PointLightComponent>().intensity =
          glm::mix(lantern.dayIntensity, lantern.nightIntensity, m_darkness);
    }
    if (m_fireflies.isValid() &&
        m_fireflies.hasComponent<sfs::ParticleEmitterComponent>())
      m_fireflies.getComponent<sfs::ParticleEmitterComponent>().enabled =
          m_darkness > 0.45f;
  }

  // Enough pins down: a spark appears where they stood.
  void checkPins()
  {
    if (m_strike)
      return;
    int down = 0;
    glm::vec3 where(0.0f);
    int count = 0;
    for (sfs::Entity& pin : scene().view<Pin, sfs::TransformComponent>())
    {
      const auto& transform = pin.getComponent<sfs::TransformComponent>();
      const glm::vec3 up = transform.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
      down += up.z < 0.55f ? 1 : 0;
      where += transform.position;
      ++count;
    }
    if (count == 0 || down < std::min(pinsForStrike, count))
      return;
    m_strike = true;
    where /= static_cast<float>(count);
    where.z = 1.2f;
    spawnSpark(where);
    burst("firework", where + glm::vec3(0.0f, 0.0f, 1.0f));
    say("STRIKE! A spark appears.", 2.5f);
    shake(0.3f);
  }

  // A spark made in code, as the scene's are: a trigger with a floating glow.
  void spawnSpark(const glm::vec3& at)
  {
    sfs::Entity spark = scene().createEntity();
    spark.addComponent<sfs::NameComponent>(std::string("Strike Spark"));
    spark.addComponent<sfs::TransformComponent>(sfs::TransformComponent(at));
    sfs::SphereCollider3DComponent trigger;
    trigger.radius = 0.7f;
    trigger.trigger = true;
    spark.addComponent<sfs::SphereCollider3DComponent>(trigger);
    spark.addComponent<Spark>();

    sfs::Entity glow = scene().createEntity();
    glow.addComponent<sfs::NameComponent>(std::string("Spark Glow"));
    glow.addComponent<sfs::TransformComponent>(
        sfs::TransformComponent(glm::vec3(0.0f)));
    glow.addComponent<sfs::MeshRendererComponent>(m_sparkLook);
    Bob bob;
    bob.amplitude = 0.25f;
    bob.speed = 1.6f;
    bob.spin = 1.2f;
    glow.addComponent<Bob>(bob);
    sfs::hierarchy::setParent(glow, spark);
    ++m_total;
  }

  void findNearStation()
  {
    m_near = nullptr;
    if (!m_hero.isValid())
      return;
    const glm::vec3 hero =
        m_hero.getComponent<sfs::TransformComponent>().position;
    float best = 1e9f;
    for (sfs::Entity& sign : scene().view<Station, sfs::TransformComponent>())
    {
      const Station& station = sign.getComponent<Station>();
      const float d = glm::length(
          glm::vec2(sign.getComponent<sfs::TransformComponent>().position) -
          glm::vec2(hero));
      if (d < station.radius && d < best)
      {
        best = d;
        m_near = &station;
      }
    }
  }

  void registerEffects();

  sfs::Entity m_hero;
  sfs::Entity m_fireflies;
  FollowCamera* m_camera = nullptr;
  sfs::MeshRendererComponent m_sparkLook; // copied from the scene's sparks
  int m_collected = 0;
  int m_total = 0;
  float m_clock = 0.0f;
  float m_time = 0.0f;
  float m_darkness = 0.0f;
  bool m_fastTime = false;
  bool m_strike = false;
  float m_celebrate = 0.0f;
  float m_fireworkClock = 0.0f;
  std::string m_message;
  float m_messageClock = 0.0f;
  const Station* m_near = nullptr;
};

#include "IslandHud.h"

inline void IslandDirector::onCreate(sfs::Scene& scene)
{
  m_hero = scene.findEntity("Hero");
  m_fireflies = scene.findEntity("Fireflies");
  m_camera = scene.tryFindObject<FollowCamera>();
  m_time = startTime;
  registerEffects();

  // The sparks to find: those in the scene, and the strike spark to come.
  for (sfs::Entity& spark : scene.view<Spark>())
    m_total += spark.getComponent<Spark>().value;
  for (sfs::Entity& glow : scene.findEntities("Spark Glow"))
    if (glow.hasComponent<sfs::MeshRendererComponent>())
    {
      m_sparkLook = glow.getComponent<sfs::MeshRendererComponent>();
      break;
    }
  if (!scene.view<Pin>().empty())
    ++m_total;

  say("Find the " + std::to_string(m_total) + " sparks!", 4.0f);
  scene.createObject<IslandHud>().setDirector(this);
}

inline void IslandDirector::registerEffects()
{
  sfs::ParticleModule* fx = particles();
  if (!fx)
    return;
  using sfs::Curve;
  using sfs::FloatRange;
  using sfs::Gradient;

  // A spark taken: a fountain of gold.
  sfs::ParticleEffectDesc sparkle;
  sparkle.burstCount = 60;
  sparkle.lifetime = FloatRange::of(0.6f, 1.3f);
  sparkle.speed = FloatRange::of(1.5f, 5.0f);
  sparkle.launchHeightSpeed = FloatRange::of(2.0f, 7.0f);
  sparkle.gravityZ = -7.0f;
  sparkle.drag = 0.6f;
  sparkle.size = FloatRange::of(0.12f, 0.3f);
  sparkle.sizeOverLife = Curve::linear(1.0f, 0.1f);
  sparkle.colorOverLife =
      Gradient::twoStop({1.0f, 0.95f, 0.6f}, {1.0f, 0.5f, 0.1f});
  sparkle.blend = sfs::BlendMode::Additive;
  fx->registerEffect("sparkle", sparkle);

  // Footfalls: a puff of dust.
  sfs::ParticleEffectDesc dust;
  dust.burstCount = 14;
  dust.shape = sfs::EmissionShape::Circle;
  dust.shapeRadius = 0.3f;
  dust.lifetime = FloatRange::of(0.4f, 0.8f);
  dust.speed = FloatRange::of(0.6f, 2.0f);
  dust.launchHeightSpeed = FloatRange::of(0.3f, 1.2f);
  dust.gravityZ = -1.5f;
  dust.drag = 2.0f;
  dust.size = FloatRange::of(0.25f, 0.5f);
  dust.sizeOverLife = Curve::linear(0.6f, 1.4f);
  dust.alphaOverLife = Curve::linear(0.55f, 0.0f);
  dust.colorOverLife = Gradient::constant({0.85f, 0.8f, 0.7f});
  fx->registerEffect("dust", dust);

  // The double jump: a ring of light.
  sfs::ParticleEffectDesc puff = dust;
  puff.burstCount = 18;
  puff.launchHeightSpeed = FloatRange::of(-0.5f, 0.2f);
  puff.speed = FloatRange::of(2.0f, 3.0f);
  puff.colorOverLife =
      Gradient::twoStop({0.6f, 0.95f, 1.0f}, {0.3f, 0.5f, 1.0f});
  puff.blend = sfs::BlendMode::Additive;
  puff.size = FloatRange::of(0.12f, 0.22f);
  fx->registerEffect("puff", puff);

  // A dash: a streak of blue sparks.
  sfs::ParticleEffectDesc dash = puff;
  dash.burstCount = 26;
  dash.speed = FloatRange::of(0.5f, 2.5f);
  dash.launchHeightSpeed = FloatRange::of(-0.5f, 1.5f);
  dash.lifetime = FloatRange::of(0.25f, 0.5f);
  fx->registerEffect("dash", dash);

  // The fountain: water arcing up and falling back (a steady emitter).
  sfs::ParticleEffectDesc fountain;
  fountain.emitRate = 70.0f;
  fountain.lifetime = FloatRange::of(0.8f, 1.1f);
  fountain.speed = FloatRange::of(0.4f, 1.1f);
  fountain.launchHeightSpeed = FloatRange::of(4.0f, 5.2f);
  fountain.gravityZ = -9.0f;
  fountain.size = FloatRange::of(0.1f, 0.2f);
  fountain.colorOverLife =
      Gradient::twoStop({0.8f, 0.95f, 1.0f}, {0.4f, 0.7f, 1.0f});
  fountain.alphaOverLife = Curve::linear(0.85f, 0.3f);
  fountain.maxParticles = 256;
  fx->registerEffect("fountain", fountain);

  // Fireflies: slow green-gold lights drifting through the grove at night.
  sfs::ParticleEffectDesc fireflies;
  fireflies.emitRate = 7.0f;
  fireflies.shape = sfs::EmissionShape::Circle;
  fireflies.shapeRadius = 7.0f;
  fireflies.lifetime = FloatRange::of(4.0f, 7.0f);
  fireflies.speed = FloatRange::of(0.1f, 0.4f);
  fireflies.launchHeightSpeed = FloatRange::of(-0.1f, 0.25f);
  fireflies.startHeight = FloatRange::of(0.4f, 2.8f);
  fireflies.size = FloatRange::of(0.07f, 0.12f);
  fireflies.colorOverLife = Gradient::constant({0.75f, 1.0f, 0.35f});
  fireflies.alphaOverLife =
      Curve::constant(0.0f).add(0.15f, 1.0f).add(0.85f, 1.0f).add(1.0f, 0.0f);
  fireflies.blend = sfs::BlendMode::Additive;
  fireflies.maxParticles = 128;
  fx->registerEffect("fireflies", fireflies);

  // The finale (and a strike): bursts of colour high over the island.
  sfs::ParticleEffectDesc firework;
  firework.burstCount = 120;
  firework.lifetime = FloatRange::of(1.0f, 1.8f);
  firework.speed = FloatRange::of(2.0f, 7.0f);
  firework.launchHeightSpeed = FloatRange::of(-4.0f, 6.0f);
  firework.gravityZ = -3.0f;
  firework.drag = 0.8f;
  firework.size = FloatRange::of(0.15f, 0.3f);
  firework.sizeOverLife = Curve::linear(1.0f, 0.2f);
  firework.colorOverLife = Gradient::constant({1.0f, 0.4f, 0.8f})
                               .add(0.4f, {0.5f, 0.8f, 1.0f})
                               .add(1.0f, {1.0f, 0.9f, 0.4f});
  firework.blend = sfs::BlendMode::Additive;
  firework.maxParticles = 256;
  fx->registerEffect("firework", firework);
}
