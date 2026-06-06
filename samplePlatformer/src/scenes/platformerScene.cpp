#include "scenes/platformerScene.h"

#include "components/platformerComponents.h"
#include "config.h"
#include "gameObjects/player.h"
#include "spells.h"
#include "systems/bloodSystem.h"
#include "systems/bulletSystem.h"
#include "systems/enemySpawnerSystem.h"
#include "systems/enemySystem.h"
#include "systems/lifetimeSystem.h"
#include "systems/pickupSystem.h"
#include "systems/platformerPhysicsSystem.h"

#include "engine/core/components/lightEmitterComponent.h"
#include "engine/core/components/particleEmitterComponent.h"
#include "engine/core/components/renderLayerComponent.h"
#include "engine/core/components/rigidBodyComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/tags/solidObject.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/particles/particleEffect.h"
#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/rendering/flatRenderContext.h"
#include "engine/runtime/rendering/modules/particles.h"
#include "engine/runtime/systems/flatRenderSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/trigonometric.hpp"

#include <SDL_rect.h>
#include <string>

using FlatParticles = sfs::Particles<sfs::FlatRenderContext>;

namespace
{
// A small spark fountain authored in the flat world's pixel units (1 unit = 1
// px): planar gravity pulls sparks down (+Y), additive blend makes them glow.
// Drives the engine's generic Particles module on the flat render path.
sfs::ParticleEffectDesc makeSparkleEffect()
{
  sfs::ParticleEffectDesc desc;
  desc.shape = sfs::EmissionShape::Circle;
  desc.shapeRadius = 6.0f;
  desc.emitRate = 50.0f;
  desc.lifetime = {0.5f, 1.2f};
  desc.speed = {40.0f, 110.0f};
  desc.size = {7.0f, 14.0f};
  desc.gravity = {0.0f, 160.0f}; // +Y is down in the flat world
  desc.drag = 1.2f;
  desc.sizeOverLife = sfs::Curve::linear(1.0f, 0.2f);
  desc.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{1.0f, 0.9f, 0.5f}, glm::vec3{1.0f, 0.35f, 0.1f});
  desc.blend = sfs::BlendMode::Additive;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot"; // soft round texture added by Game::setup
  desc.maxParticles = 256;
  return desc;
}

// Blood burst fired on a bullet hit. Sizes/speeds are in the flat world's pixel
// units (worldUnitToPixels == 1), so they're large enough to read as a spray.
// One-shot via spawnBurst, so it uses burstCount, not a continuous emit rate.
sfs::ParticleEffectDesc makeBloodEffect()
{
  sfs::ParticleEffectDesc desc;
  desc.shape = sfs::EmissionShape::Point;
  desc.burstCount = 52;
  desc.lifetime = {0.45f, 1.2f};
  desc.speed = {200.0f, 640.0f};
  desc.size = {12.0f, 34.0f};
  desc.gravity = {0.0f, 1400.0f}; // +Y down: blood arcs and falls
  desc.drag = 0.4f;
  desc.sizeOverLife = sfs::Curve::linear(1.0f, 0.45f);
  desc.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{0.9f, 0.02f, 0.02f}, glm::vec3{0.4f, 0.0f, 0.0f});
  desc.blend = sfs::BlendMode::Alpha;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 256;
  return desc;
}

// A heavier gib burst fired when an enemy dies -- more, bigger, faster than the
// per-hit spray.
sfs::ParticleEffectDesc makeGoreEffect()
{
  sfs::ParticleEffectDesc desc;
  desc.shape = sfs::EmissionShape::Point;
  desc.burstCount = 110;
  desc.lifetime = {0.5f, 1.7f};
  desc.speed = {260.0f, 900.0f};
  desc.size = {14.0f, 50.0f};
  desc.gravity = {0.0f, 1500.0f};
  desc.drag = 0.25f;
  desc.sizeOverLife = sfs::Curve::linear(1.0f, 0.5f);
  desc.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{0.95f, 0.05f, 0.05f}, glm::vec3{0.3f, 0.0f, 0.0f});
  desc.blend = sfs::BlendMode::Alpha;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 256;
  return desc;
}

// Explosive impact: a bright additive fireball.
sfs::ParticleEffectDesc makeExplosionEffect()
{
  sfs::ParticleEffectDesc desc;
  desc.shape = sfs::EmissionShape::Point;
  desc.burstCount = 70;
  desc.lifetime = {0.35f, 0.9f};
  desc.speed = {220.0f, 760.0f};
  desc.size = {16.0f, 46.0f};
  desc.gravity = {0.0f, 500.0f};
  desc.drag = 1.4f;
  desc.sizeOverLife = sfs::Curve::linear(1.0f, 0.3f);
  desc.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{1.0f, 0.9f, 0.4f}, glm::vec3{1.0f, 0.25f, 0.0f});
  desc.blend = sfs::BlendMode::Additive;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 256;
  return desc;
}

// Chain-lightning / ricochet spark: a quick electric-blue additive pop.
sfs::ParticleEffectDesc makeSparkEffect()
{
  sfs::ParticleEffectDesc desc;
  desc.shape = sfs::EmissionShape::Point;
  desc.burstCount = 14;
  desc.lifetime = {0.15f, 0.45f};
  desc.speed = {120.0f, 360.0f};
  desc.size = {4.0f, 11.0f};
  desc.drag = 2.0f;
  desc.sizeOverLife = sfs::Curve::linear(1.0f, 0.2f);
  desc.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{0.7f, 0.9f, 1.0f}, glm::vec3{0.2f, 0.45f, 1.0f});
  desc.blend = sfs::BlendMode::Additive;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 128;
  return desc;
}

// Spell pickup collected: a bright white-gold pop.
sfs::ParticleEffectDesc makePickupEffect()
{
  sfs::ParticleEffectDesc desc;
  desc.shape = sfs::EmissionShape::Circle;
  desc.shapeRadius = 8.0f;
  desc.burstCount = 28;
  desc.lifetime = {0.4f, 0.9f};
  desc.speed = {80.0f, 300.0f};
  desc.size = {6.0f, 16.0f};
  desc.drag = 1.5f;
  desc.sizeOverLife = sfs::Curve::linear(1.0f, 0.2f);
  desc.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(
      glm::vec3{1.0f, 1.0f, 0.85f}, glm::vec3{1.0f, 0.8f, 0.3f});
  desc.blend = sfs::BlendMode::Additive;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 128;
  return desc;
}

// Continuous aura around a floating spell orb (emitter component).
sfs::ParticleEffectDesc makeAuraEffect()
{
  sfs::ParticleEffectDesc desc;
  desc.shape = sfs::EmissionShape::Circle;
  desc.shapeRadius = 18.0f;
  desc.emitRate = 26.0f;
  desc.lifetime = {0.5f, 1.1f};
  desc.speed = {6.0f, 26.0f};
  desc.size = {4.0f, 10.0f};
  desc.gravity = {0.0f, -40.0f}; // drift upward
  desc.drag = 1.0f;
  desc.sizeOverLife = sfs::Curve::linear(0.6f, 1.0f);
  desc.alphaOverLife = sfs::Curve::linear(0.9f, 0.0f);
  desc.colorOverLife = sfs::Gradient::constant(glm::vec3{1.0f, 1.0f, 1.0f});
  desc.blend = sfs::BlendMode::Additive;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 128;
  return desc;
}
} // namespace

void PlatformerScene::onInit()
{
  createEntities();

  // Enemy patrol runs before physics so the patrol velocity is integrated this
  // frame; bullets advance + resolve hits before the particle sim runs.
  addSystem<platformer::EnemySystem>();
  addSystem<platformer::PlatformerPhysicsSystem>();
  auto& bullets = addSystem<platformer::BulletSystem>();
  // Physics blood runs right after bullets (so droplets sprayed this frame
  // integrate immediately) and before the renderer (which draws + animates the
  // stuck decals).
  auto& blood = addSystem<platformer::BloodSystem>();
  addSystem<platformer::LifetimeSystem>(); // expires muzzle/death flash lights

  auto& render = addSystem<sfs::FlatRenderSystem>(m_assetStore, quadRenderer());

  // Dark, moody base so the point lights (player lamp, torches, muzzle and
  // death flashes) carry the scene -- gunfire and kills light the room.
  sfs::FlatLighting lighting;
  lighting.ambient = 0.28f;
  lighting.ambientColor = glm::vec3{0.32f, 0.36f, 0.5f};
  lighting.diffuseStrength = 0.0f;
  render.setLighting(lighting);
  render.setMaxDecals(24000); // global backstop (bounds memory/frame cost)
  // Gore-heavy: a 12px cell holds up to 16 blood marks, then that spot is
  // saturated and stops accepting more -- so platforms cake up and STAY bloody
  // (no churn) instead of old blood being evicted as new blood lands.
  render.setDecalCoverage(12.0f, 16);

  auto& particles = render.withModule<FlatParticles>();
  // This game is gore-heavy (big splatter + persistent stains + drips), so it
  // lifts the engine's default particle ceiling -- it has the frame headroom.
  particles.setMaxParticles(14000);
  particles.registerEffect("sparkle", makeSparkleEffect());
  particles.registerEffect("blood", makeBloodEffect());
  particles.registerEffect("gore", makeGoreEffect());
  particles.registerEffect("explosion", makeExplosionEffect());
  particles.registerEffect("spark", makeSparkEffect());
  particles.registerEffect("pickup", makePickupEffect());
  particles.registerEffect("aura", makeAuraEffect());

  // Spell pickups: collecting an orb appends its spell to the player's loadout.
  addSystem<platformer::PickupSystem>().setParticles(&particles);

  // Bullet hits/effects fire through the same particle module; kills bump the
  // score, shake the screen, flash, and sometimes drop a spell.
  bullets.setParticles(&particles);
  bullets.setBlood(&blood); // hits/kills spray physics blood through this

  // Physics blood: droplets fly with gravity and stick to platforms as decals.
  blood.setRenderer(&render);             // stamps the stuck decals
  blood.setSprite(m_orbSprite);           // soft glow -> splatter blobs
  blood.setSolidSprite(m_platformSprite); // hard pixel -> thin sub-streaks
  bullets.setOnKill(
      [this](glm::vec2 pos)
      {
        ++m_kills;
        m_shake = SHAKE_ON_KILL;
        spawnFlash(pos, glm::vec3{1.0f, 0.3f, 0.15f}, 340.0f, DEATH_FLASH_TIME);

        std::uniform_real_distribution<float> roll(0.0f, 1.0f);
        if (roll(m_rng) < SPELL_DROP_CHANCE)
          dropSpell(pos);
      });

  // Constant respawn for testing (capped); reuses createEnemy via a callback.
  addSystem<platformer::EnemySpawnerSystem>().setSpawn([this]
                                                       { spawnRandomEnemy(); });
}

void PlatformerScene::createEntities()
{
  // Sprites registered up front so the player/enemies look them up by name.
  m_assetStore.addTexture(
      "platformer_sheet", ASSET_ROOT + "spriteSheets/tilemap.png");
  m_guySprite = m_assetStore.addSpriteFromSheet(
      "platformer_sheet", "guy", 16, 16, 16, 6, 1, 0);
  // 1x1 white sprite scaled per-platform via the transform.
  m_platformSprite =
      m_assetStore.addSprite("white_pixel", "platform", SDL_Rect{0, 0, 1, 1});
  // Soft round glow, used both as the laser bolt streak and the spell orb.
  m_orbSprite =
      m_assetStore.addSprite("white_dot", "bolt", SDL_Rect{0, 0, 32, 32});

  generatePlatforms();

  m_player = &createObject<Player>();

  // Coloured torches on a few generated platforms; they light the dark level
  // and emit sparks (generic Particles module + point lighting on the flat
  // path).
  if (m_platforms.size() > 4)
  {
    const glm::vec3 colors[] = {
        {1.0f, 0.55f, 0.2f}, {0.4f, 0.7f, 1.0f}, {1.0f, 0.4f, 0.7f}};
    for (int i = 0; i < 3; ++i)
    {
      const glm::vec4 p = m_platforms[(i * 5 + 2) % m_platforms.size()];
      createTorch({p.x, p.y - p.w - 18.0f}, colors[i]);
    }
  }
}

void PlatformerScene::generatePlatforms()
{
  // Ground spans the world so nothing falls into the void.
  const glm::vec2 groundCenter{WORLD_WIDTH * 0.5f, GROUND_Y};
  const glm::vec2 groundSize{WORLD_WIDTH + 400.0f, 110.0f};
  createPlatform(groundCenter, groundSize);
  m_platforms.push_back({groundCenter.x,
                         groundCenter.y,
                         groundSize.x * 0.5f,
                         groundSize.y * 0.5f});

  std::uniform_real_distribution<float> dx(140.0f, WORLD_WIDTH - 140.0f);
  std::uniform_real_distribution<float> dy(
      GROUND_Y - 520.0f, GROUND_Y - 150.0f);
  std::uniform_real_distribution<float> dw(110.0f, 300.0f);

  for (int i = 0; i < PLATFORM_COUNT; ++i)
  {
    const glm::vec2 center{dx(m_rng), dy(m_rng)};
    const glm::vec2 size{
        dw(m_rng), 64.0f}; // chunky blocks: a face for blood to drip down
    const float halfX = size.x * 0.5f;

    // Skip a platform that would overlap one already placed nearby.
    bool ok = true;
    for (const auto& p : m_platforms)
    {
      if (glm::abs(p.x - center.x) < p.z + halfX + 50.0f &&
          glm::abs(p.y - center.y) < 80.0f)
      {
        ok = false;
        break;
      }
    }
    if (!ok)
      continue;

    createPlatform(center, size);
    m_platforms.push_back({center.x, center.y, halfX, size.y * 0.5f});
  }
}

void PlatformerScene::createTorch(const glm::vec2& pos, const glm::vec3& color)
{
  createEntity()
      .addComponent<sfs::TransformComponent>(pos)
      .addComponent<sfs::LightEmitterComponent>(460.0f, 1.7f, 0.0f, color)
      .addComponent<sfs::ParticleEmitterComponent>(
          "sparkle", glm::vec2{0.0f, 0.0f}, 0.0f);
}

void PlatformerScene::spawnFlash(const glm::vec2& pos,
                                 const glm::vec3& color,
                                 float radius,
                                 float time)
{
  createEntity()
      .addComponent<sfs::TransformComponent>(pos)
      .addComponent<sfs::LightEmitterComponent>(radius, 2.6f, 0.0f, color)
      .addComponent<platformer::Lifetime>(time);
}

void PlatformerScene::createPlatform(const glm::vec2& center,
                                     const glm::vec2& size)
{
  createEntity()
      .addComponent<sfs::SpriteComponent>(
          m_platformSprite, glm::vec2{0.5f, 0.5f})
      .addComponent<sfs::TransformComponent>(center, size)
      .addComponent<sfs::BoxCollider2D>(size * 0.5f)
      .addTag<sfs::SolidObject>()
      .addComponent<sfs::RenderLayerComponent>(0);
}

void PlatformerScene::createEnemy(const glm::vec2& center)
{
  createEntity()
      .addComponent<sfs::SpriteComponent>(m_guySprite, glm::vec2{0.5f, 0.5f})
      .addComponent<sfs::TransformComponent>(center, glm::vec2{2.5f, 2.5f})
      .addComponent<sfs::RigidBodyComponent>(glm::vec2{0.0f, 0.0f})
      .addComponent<sfs::BoxCollider2D>(glm::vec2{18.0f, 20.0f})
      .addComponent<platformer::PlatformerBody>()
      .addComponent<platformer::Enemy>(ENEMY_HEALTH)
      .addComponent<sfs::RenderLayerComponent>(9);
}

void PlatformerScene::spawnRandomEnemy()
{
  if (m_platforms.empty())
    return;

  std::uniform_int_distribution<std::size_t> pick(0, m_platforms.size() - 1);
  const glm::vec4 p = m_platforms[pick(m_rng)];

  std::uniform_real_distribution<float> ox(-p.z * 0.7f, p.z * 0.7f);
  const float top = p.y - p.w; // platform top (Y grows downward)
  createEnemy({p.x + ox(m_rng), top - 22.0f}); // rest just above the surface
}

glm::vec2 PlatformerScene::cameraTarget() const
{
  if (!m_player)
    return m_shakeOffset;

  return m_player->entity().getComponent<sfs::TransformComponent>().position +
         m_shakeOffset;
}

void PlatformerScene::onUpdate(double deltaTime)
{
  if (deltaTime > 0.0001)
    m_fps += (1.0 / deltaTime - m_fps) * 0.1;

  // Decay screen shake and pick this frame's camera jolt.
  m_shakeTime += deltaTime;
  m_shake =
      glm::max(0.0f, m_shake - SHAKE_DECAY * static_cast<float>(deltaTime));
  m_shakeOffset = {glm::sin(static_cast<float>(m_shakeTime) * 73.0f) * m_shake,
                   glm::cos(static_cast<float>(m_shakeTime) * 91.0f) * m_shake};
}

void PlatformerScene::onRender()
{
  textRenderer().drawText(
      20, 20, "Move: A / D   Jump: Space   Hold L-click: shoot");
  textRenderer().drawText(20, 44, "KILLS: " + std::to_string(m_kills));
  textRenderer().drawText(
      20, 68, "FPS: " + std::to_string(static_cast<int>(m_fps)));

  // Active spell loadout (collected from enemy drops).
  if (m_player)
  {
    const auto& spells =
        m_player->entity().getComponent<platformer::Loadout>().spells;

    if (spells.empty())
    {
      textRenderer().drawText(
          20, 92, "SPELLS: none - kill enemies for spell orbs");
    }
    else
    {
      int counts[static_cast<int>(platformer::Spell::Count)] = {0};
      for (const platformer::Spell s : spells)
        ++counts[static_cast<int>(s)];

      std::string line = "SPELLS: ";
      for (int i = 0; i < static_cast<int>(platformer::Spell::Count); ++i)
      {
        if (counts[i] == 0)
          continue;
        line += platformer::spellName(static_cast<platformer::Spell>(i));
        if (counts[i] > 1)
          line += "x" + std::to_string(counts[i]);
        line += " ";
      }
      textRenderer().drawText(20, 92, line);
    }
  }
}

void PlatformerScene::dropSpell(const glm::vec2& pos)
{
  std::uniform_int_distribution<int> pick(
      0, static_cast<int>(platformer::Spell::Count) - 1);
  const platformer::Spell spell = static_cast<platformer::Spell>(pick(m_rng));

  createEntity()
      .addComponent<sfs::TransformComponent>(pos, glm::vec2{1.2f, 1.2f})
      .addComponent<sfs::SpriteComponent>(m_orbSprite, glm::vec2{0.5f, 0.5f})
      .addComponent<sfs::LightEmitterComponent>(
          300.0f, 2.0f, 0.0f, platformer::spellColor(spell))
      .addComponent<sfs::ParticleEmitterComponent>(
          "aura", glm::vec2{0.0f, 0.0f}, 0.0f)
      .addComponent<platformer::SpellPickup>(spell)
      .addComponent<sfs::RenderLayerComponent>(8);
}
