#pragma once

#include "components/platformerComponents.h"
#include "config.h"
#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/lightEmitterComponent.h"
#include "engine/core/components/renderLayerComponent.h"
#include "engine/core/components/rigidBodyComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/sceneManager/scene.h"
#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"
#include "spells.h"

#include <algorithm>

// The player: walks (A/D), jumps (Space), and sprays laser bolts toward the
// mouse (hold left-click). Each shot is built from the player's collected
// spells (Loadout), stacking modifiers
// (Triple/Bounce/Homing/Pierce/Explosive/Chain/ Giant/Gravity/DamageUp/Rapid)
// onto every bolt.
class Player : public sfs::GameObject
{
public:
  void onCreate(sfs::Scene& scene) override
  {
    m_scene = &scene;

    const sfs::Sprite* guy = scene.assetStore().getSprite("guy");
    const sfs::Sprite* bolt = scene.assetStore().getSprite("bolt");
    m_boltSprite = bolt ? bolt->id : 0;

    m_entity = scene.createEntity()
                   .addComponent<sfs::SpriteComponent>(
                       guy ? guy->id : 0, glm::vec2{0.5f, 0.5f})
                   .addComponent<sfs::TransformComponent>(
                       glm::vec2{PLAYER_START_X, PLAYER_START_Y},
                       glm::vec2{3.0f, 3.0f})
                   .addComponent<sfs::RigidBodyComponent>(glm::vec2{0.0f, 0.0f})
                   .addComponent<sfs::BoxCollider2D>(glm::vec2{20.0f, 24.0f})
                   .addComponent<platformer::PlatformerBody>()
                   .addComponent<platformer::PlayerTag>()
                   .addComponent<platformer::Loadout>()
                   .addComponent<sfs::LightEmitterComponent>(
                       380.0f, 1.3f, 0.0f, glm::vec3{1.0f, 0.85f, 0.55f})
                   .addComponent<sfs::RenderLayerComponent>(10);
  }

  void onUpdate(double deltaTime) override
  {
    if (m_fireCooldown > 0.0)
      m_fireCooldown -= deltaTime;
  }

  void onProcessInput(const sfs::Input& input) override
  {
    auto& body = m_entity.getComponent<sfs::RigidBodyComponent>();
    auto& state = m_entity.getComponent<platformer::PlatformerBody>();

    float vx = 0.0f;
    if (input.keyboard().keyHeld(sfs::Key::A))
      vx -= MOVE_SPEED;
    if (input.keyboard().keyHeld(sfs::Key::D))
      vx += MOVE_SPEED;
    body.velocity.x = vx;

    if (state.onGround && input.keyboard().keyPressed(sfs::Key::Space))
      body.velocity.y = -JUMP_SPEED;

    if (input.mouse().mouseHeld(sfs::MouseButton::Left) &&
        m_fireCooldown <= 0.0)
    {
      const auto& spells = m_entity.getComponent<platformer::Loadout>().spells;
      fire(input.mouse().getPosition(), spells);

      const int rapid = static_cast<int>(
          std::count(spells.begin(), spells.end(), platformer::Spell::Rapid));
      m_fireCooldown = glm::max(
          0.02, FIRE_INTERVAL * glm::pow(0.62, static_cast<double>(rapid)));
    }
  }

private:
  static glm::vec2 rotate(const glm::vec2& v, float radians)
  {
    const float s = glm::sin(radians);
    const float c = glm::cos(radians);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
  }

  void fire(const glm::vec2& mouseScreen,
            const std::vector<platformer::Spell>& spells)
  {
    const glm::vec2 screenCenter{WINDOW_WIDTH * 0.5f, WINDOW_HEIGHT * 0.5f};
    glm::vec2 aim = mouseScreen - screenCenter;
    const float len = glm::length(aim);
    aim = len > 0.001f ? aim / len : glm::vec2{1.0f, 0.0f};

    // Accumulate the stacked modifiers from the loadout.
    int shots = 1;
    glm::vec3 colorSum{0.0f};
    int colorCount = 0;

    platformer::Bullet bullet;
    bullet.life = BULLET_LIFE;
    bullet.damage = BULLET_DAMAGE;
    float sizeMul = 1.0f;
    float speedMul = 1.0f;

    for (const platformer::Spell spell : spells)
    {
      switch (spell)
      {
      case platformer::Spell::Triple:
        shots += 2;
        break;
      case platformer::Spell::Bounce:
        bullet.bounces += BOUNCES_PER_SPELL;
        break;
      case platformer::Spell::Homing:
        bullet.homing = true;
        break;
      case platformer::Spell::Pierce:
        bullet.pierce = true;
        break;
      case platformer::Spell::Explosive:
        bullet.explosive = true;
        break;
      case platformer::Spell::Chain:
        bullet.chain = true;
        break;
      case platformer::Spell::Gravity:
        bullet.gravity += GRAVITY_PER_SPELL;
        break;
      case platformer::Spell::DamageUp:
        bullet.damage += DAMAGE_PER_SPELL;
        break;
      case platformer::Spell::Rapid:
        break; // affects fire cooldown
      default:
        break;
      }
      colorSum += spellColor(spell);
      ++colorCount;
    }

    bullet.color = colorCount > 0 ? colorSum / static_cast<float>(colorCount)
                                  : glm::vec3{1.0f, 0.2f, 0.12f};
    shots = glm::min(shots, 13);

    // Bounce / homing bolts live longer so they have time to ricochet / curve.
    if (bullet.bounces > 0 || bullet.homing)
      bullet.life *= 2.6f;

    const glm::vec2 origin =
        m_entity.getComponent<sfs::TransformComponent>().position;
    const float speed = BULLET_SPEED * speedMul;
    const float spreadStep = SPREAD_DEGREES * 0.01745329f; // deg -> rad

    for (int i = 0; i < shots; ++i)
    {
      const float offset =
          (static_cast<float>(i) - (shots - 1) * 0.5f) * spreadStep;
      const glm::vec2 dir = rotate(aim, offset);

      platformer::Bullet b = bullet;
      b.velocity = dir * speed;

      m_scene->createEntity()
          .addComponent<sfs::TransformComponent>(
              origin,
              glm::vec2{1.3f * sizeMul, 0.32f * sizeMul},
              glm::atan(dir.y, dir.x))
          .addComponent<sfs::SpriteComponent>(
              m_boltSprite, glm::vec2{0.5f, 0.5f})
          .addComponent<platformer::Bullet>(b)
          .addComponent<sfs::LightEmitterComponent>(
              170.0f * sizeMul, 2.2f, 0.0f, b.color)
          .addComponent<sfs::RenderLayerComponent>(11);
    }

    // Muzzle flash, tinted by the current bolt colour.
    m_scene->createEntity()
        .addComponent<sfs::TransformComponent>(origin + aim * 30.0f)
        .addComponent<sfs::LightEmitterComponent>(
            240.0f, 2.4f, 0.0f, bullet.color)
        .addComponent<platformer::Lifetime>(MUZZLE_FLASH_TIME);
  }

  sfs::Scene* m_scene = nullptr;
  sfs::SpriteId m_boltSprite = 0;
  double m_fireCooldown = 0.0;
};
