#pragma once

#include "components/platformerComponents.h"
#include "config.h"
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
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/geometric.hpp"

#include <cmath>

// The player: walks (A/D), jumps (Space), and sprays laser bolts toward the
// mouse (hold left-click). The camera centres on the player, so at zoom 1 the
// player sits at screen centre and the aim direction is (mouse - screenCentre).
class Player : public sfs::GameObject
{
public:
  void onCreate(sfs::Scene& scene) override
  {
    m_scene = &scene;

    const sfs::Sprite* guy = scene.assetStore().getSprite("guy");
    const sfs::Sprite* bolt = scene.assetStore().getSprite("bolt");
    m_boltSprite = bolt ? bolt->id : 0;

    m_entity =
        scene.createEntity()
            .addComponent<sfs::SpriteComponent>(guy ? guy->id : 0,
                                                glm::vec2{0.5f, 0.5f})
            .addComponent<sfs::TransformComponent>(
                glm::vec2{PLAYER_START_X, PLAYER_START_Y},
                glm::vec2{3.0f, 3.0f})
            .addComponent<sfs::RigidBodyComponent>(glm::vec2{0.0f, 0.0f})
            .addComponent<platformer::BoxCollider>(glm::vec2{20.0f, 24.0f})
            .addComponent<platformer::PlatformerBody>()
            .addComponent<platformer::PlayerTag>()
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

    if (input.mouse().mouseHeld(sfs::MouseButton::Left) && m_fireCooldown <= 0.0)
    {
      shoot(input.mouse().getPosition());
      m_fireCooldown = FIRE_INTERVAL;
    }
  }

private:
  void shoot(const glm::vec2& mouseScreen)
  {
    const glm::vec2 screenCenter{WINDOW_WIDTH * 0.5f, WINDOW_HEIGHT * 0.5f};
    glm::vec2 dir = mouseScreen - screenCenter;
    const float len = glm::length(dir);
    dir = len > 0.001f ? dir / len : glm::vec2{1.0f, 0.0f};

    const glm::vec2 origin =
        m_entity.getComponent<sfs::TransformComponent>().position;

    // Laser bolt: a stretched soft-glow sprite carrying its own bright red
    // light, so the bolt is self-lit and lights the environment as it streaks.
    // Rotated to align the streak with its travel direction.
    const double angle = std::atan2(dir.y, dir.x);
    m_scene->createEntity()
        .addComponent<sfs::TransformComponent>(origin, glm::vec2{1.3f, 0.32f},
                                               angle)
        .addComponent<sfs::SpriteComponent>(m_boltSprite, glm::vec2{0.5f, 0.5f})
        .addComponent<platformer::Bullet>(dir * BULLET_SPEED, BULLET_LIFE)
        .addComponent<sfs::LightEmitterComponent>(
            170.0f, 2.2f, 0.0f, glm::vec3{1.0f, 0.2f, 0.12f})
        .addComponent<sfs::RenderLayerComponent>(11);

    // Muzzle flash: a brief bright light at the gun.
    m_scene->createEntity()
        .addComponent<sfs::TransformComponent>(origin + dir * 30.0f)
        .addComponent<sfs::LightEmitterComponent>(
            240.0f, 2.4f, 0.0f, glm::vec3{1.0f, 0.45f, 0.25f})
        .addComponent<platformer::Lifetime>(MUZZLE_FLASH_TIME);
  }

  sfs::Scene* m_scene = nullptr;
  sfs::SpriteId m_boltSprite = 0;
  double m_fireCooldown = 0.0;
};
