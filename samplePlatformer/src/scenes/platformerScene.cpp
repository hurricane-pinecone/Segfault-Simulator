#include "scenes/platformerScene.h"

#include "components/platformerComponents.h"
#include "config.h"
#include "gameObjects/player.h"
#include "systems/bulletSystem.h"
#include "systems/enemySpawnerSystem.h"
#include "systems/enemySystem.h"
#include "systems/lifetimeSystem.h"
#include "systems/platformerPhysicsSystem.h"

#include "engine/core/components/lightEmitterComponent.h"
#include "engine/core/components/particleEmitterComponent.h"
#include "engine/core/components/renderLayerComponent.h"
#include "engine/core/components/rigidBodyComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/particles/particleEffect.h"
#include "engine/runtime/TextRenderer/textRenderer.h"
#include "engine/runtime/rendering/flatRenderContext.h"
#include "engine/runtime/rendering/modules/particles.h"
#include "engine/runtime/systems/flatRenderSystem.h"

#include <SDL_rect.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
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
  desc.colorOverLife = sfs::Gradient::twoStop(glm::vec3{1.0f, 0.9f, 0.5f},
                                              glm::vec3{1.0f, 0.35f, 0.1f});
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
  desc.burstCount = 32;
  desc.lifetime = {0.4f, 1.0f};
  desc.speed = {180.0f, 520.0f};
  desc.size = {10.0f, 26.0f};
  desc.gravity = {0.0f, 1300.0f}; // +Y down: blood arcs and falls
  desc.drag = 0.4f;
  desc.sizeOverLife = sfs::Curve::linear(1.0f, 0.45f);
  desc.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(glm::vec3{0.9f, 0.02f, 0.02f},
                                              glm::vec3{0.4f, 0.0f, 0.0f});
  desc.blend = sfs::BlendMode::Alpha;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 256;
  return desc;
}

// A blood stain that settles where the blood landed and lingers, so hits leave
// a mark on the platform. Near-zero velocity + heavy drag + no gravity pins the
// cluster in place; a long lifetime makes it "stick".
sfs::ParticleEffectDesc makeBloodStainEffect()
{
  sfs::ParticleEffectDesc desc;
  // A thin horizontal smear hugging the platform surface (Box, low height), that
  // barely moves and settles at once -- so it reads as a stain on the surface.
  desc.shape = sfs::EmissionShape::Box;
  desc.boxExtents = {16.0f, 3.0f};
  desc.burstCount = 18;
  desc.lifetime = {10.0f, 20.0f};
  desc.speed = {0.0f, 6.0f};
  desc.size = {7.0f, 16.0f};
  desc.gravity = {0.0f, 0.0f};
  desc.drag = 9.0f;
  desc.sizeOverLife = sfs::Curve::constant(1.0f);
  desc.alphaOverLife = sfs::Curve::linear(0.85f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(glm::vec3{0.5f, 0.0f, 0.0f},
                                              glm::vec3{0.28f, 0.0f, 0.0f});
  desc.blend = sfs::BlendMode::Alpha;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 512;
  return desc;
}

// A heavier gib burst fired when an enemy dies -- more, bigger, faster than the
// per-hit spray.
sfs::ParticleEffectDesc makeGoreEffect()
{
  sfs::ParticleEffectDesc desc;
  desc.shape = sfs::EmissionShape::Point;
  desc.burstCount = 60;
  desc.lifetime = {0.5f, 1.4f};
  desc.speed = {240.0f, 760.0f};
  desc.size = {12.0f, 36.0f};
  desc.gravity = {0.0f, 1500.0f};
  desc.drag = 0.3f;
  desc.sizeOverLife = sfs::Curve::linear(1.0f, 0.5f);
  desc.alphaOverLife = sfs::Curve::linear(1.0f, 0.0f);
  desc.colorOverLife = sfs::Gradient::twoStop(glm::vec3{0.95f, 0.05f, 0.05f},
                                              glm::vec3{0.3f, 0.0f, 0.0f});
  desc.blend = sfs::BlendMode::Alpha;
  desc.space = sfs::SimulationSpace::World;
  desc.texture = "white_dot";
  desc.maxParticles = 256;
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
  addSystem<platformer::LifetimeSystem>(); // expires muzzle/death flash lights

  auto& render = addSystem<sfs::FlatRenderSystem>(m_assetStore, quadRenderer());

  // Dark, moody base so the point lights (player lamp, torches, muzzle and
  // death flashes) carry the scene -- gunfire and kills light the room.
  sfs::FlatLighting lighting;
  lighting.ambient = 0.28f;
  lighting.ambientColor = glm::vec3{0.32f, 0.36f, 0.5f};
  lighting.diffuseStrength = 0.0f;
  render.setLighting(lighting);

  auto& particles = render.withModule<FlatParticles>();
  particles.registerEffect("sparkle", makeSparkleEffect());
  particles.registerEffect("blood", makeBloodEffect());
  particles.registerEffect("blood_stain", makeBloodStainEffect());
  particles.registerEffect("gore", makeGoreEffect());

  // Bullet hits spray blood; kills trigger a gib burst + shake + death flash.
  bullets.setBlood(&particles);
  bullets.setOnKill(
      [this](glm::vec2 pos)
      {
        ++m_kills;
        m_shake = SHAKE_ON_KILL;
        spawnFlash(pos, glm::vec3{1.0f, 0.3f, 0.15f}, 340.0f, DEATH_FLASH_TIME);
      });

  // Constant respawn for testing (capped); reuses createEnemy via a callback.
  addSystem<platformer::EnemySpawnerSystem>().setSpawn(
      [this] { spawnRandomEnemy(); });
}

void PlatformerScene::createEntities()
{
  // Sprites registered up front so the player/enemies look them up by name.
  m_assetStore.addTexture("platformer_sheet",
                          ASSET_ROOT + "spriteSheets/tilemap.png");
  m_guySprite = m_assetStore.addSpriteFromSheet("platformer_sheet", "guy", 16,
                                                16, 16, 6, 1, 0);
  // 1x1 white sprite scaled per-platform via the transform.
  m_platformSprite =
      m_assetStore.addSprite("white_pixel", "platform", SDL_Rect{0, 0, 1, 1});
  // Laser bolt sprite: the soft round glow, stretched into a streak per-shot.
  m_assetStore.addSprite("white_dot", "bolt", SDL_Rect{0, 0, 32, 32});

  generatePlatforms();

  m_player = &createObject<Player>();

  // Coloured torches on a few generated platforms; they light the dark level
  // and emit sparks (generic Particles module + point lighting on the flat path).
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
  m_platforms.push_back({groundCenter.x, groundCenter.y, groundSize.x * 0.5f,
                         groundSize.y * 0.5f});

  std::uniform_real_distribution<float> dx(140.0f, WORLD_WIDTH - 140.0f);
  std::uniform_real_distribution<float> dy(GROUND_Y - 520.0f, GROUND_Y - 150.0f);
  std::uniform_real_distribution<float> dw(110.0f, 300.0f);

  for (int i = 0; i < PLATFORM_COUNT; ++i)
  {
    const glm::vec2 center{dx(m_rng), dy(m_rng)};
    const glm::vec2 size{dw(m_rng), 28.0f};
    const float halfX = size.x * 0.5f;

    // Skip a platform that would overlap one already placed nearby.
    bool ok = true;
    for (const auto& p : m_platforms)
    {
      if (std::fabs(p.x - center.x) < p.z + halfX + 50.0f &&
          std::fabs(p.y - center.y) < 80.0f)
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
      .addComponent<sfs::ParticleEmitterComponent>("sparkle",
                                                   glm::vec2{0.0f, 0.0f}, 0.0f);
}

void PlatformerScene::spawnFlash(const glm::vec2& pos, const glm::vec3& color,
                                 float radius, float time)
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
      .addComponent<sfs::SpriteComponent>(m_platformSprite,
                                          glm::vec2{0.5f, 0.5f})
      .addComponent<sfs::TransformComponent>(center, size)
      .addComponent<platformer::BoxCollider>(size * 0.5f)
      .addComponent<platformer::Solid>()
      .addComponent<sfs::RenderLayerComponent>(0);
}

void PlatformerScene::createEnemy(const glm::vec2& center)
{
  createEntity()
      .addComponent<sfs::SpriteComponent>(m_guySprite, glm::vec2{0.5f, 0.5f})
      .addComponent<sfs::TransformComponent>(center, glm::vec2{2.5f, 2.5f})
      .addComponent<sfs::RigidBodyComponent>(glm::vec2{0.0f, 0.0f})
      .addComponent<platformer::BoxCollider>(glm::vec2{18.0f, 20.0f})
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
  m_shake = std::max(0.0f, m_shake - SHAKE_DECAY * static_cast<float>(deltaTime));
  m_shakeOffset = {
      std::sin(static_cast<float>(m_shakeTime) * 73.0f) * m_shake,
      std::cos(static_cast<float>(m_shakeTime) * 91.0f) * m_shake};
}

void PlatformerScene::onRender()
{
  textRenderer().drawText(20, 20, "Flat 2D platformer (engine generic path)");
  textRenderer().drawText(20, 44,
                          "Move: A / D   Jump: Space   Hold L-click: shoot");
  textRenderer().drawText(20, 68, "KILLS: " + std::to_string(m_kills));
  textRenderer().drawText(
      20, 92, "FPS: " + std::to_string(static_cast<int>(m_fps)));
}
