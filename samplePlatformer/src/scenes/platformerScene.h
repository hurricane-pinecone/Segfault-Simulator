#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/ui/layer.h"
#include "engine/runtime/ui/ui.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <random>
#include <string>
#include <vector>

class Player;

// The platformer scene: builds the level (player, platforms, lit torches that
// emit sparks), runs the local physics system, and draws as a 2D game through
// the engine's renderer (a RenderSystem3D with a flat Space2D).
// The HUD: controls, kills, frame rate and spells, top left.
class PlatformerHud : public sfs::ui::Layer
{
public:
  sfs::ui::View body() override;
};

class PlatformerScene : public sfs::Scene
{
public:
  using sfs::Scene::Scene; // inherit (SceneId, SceneServices[, name]) ctors

  int kills() const { return m_kills; }
  double fps() const { return m_fps; }
  // The spell loadout, as the HUD shows it.
  std::string spellLine() const;

protected:
  void onInit() override;
  void onUpdate(double deltaTime) override;

private:
  void createEntities();
  void generatePlatforms(); // procedural ground + floating platforms
  void createPlatform(const glm::vec2& center, const glm::vec2& size);
  void createTorch(const glm::vec2& pos, const glm::vec3& color);
  void createEnemy(const glm::vec2& center);
  void spawnRandomEnemy(); // on a random generated platform (testing)

  // A short-lived point light (muzzle/death flash).
  void spawnFlash(const glm::vec2& pos,
                  const glm::vec3& color,
                  float radius,
                  float time);

  // Drop a random collectable spell orb at a kill position.
  void dropSpell(const glm::vec2& pos);

  Player* m_player = nullptr;
  sfs::Entity m_camera; // ECS camera (engine CameraSystem) following the player
  sfs::SpriteId m_platformSprite = 0;
  sfs::SpriteId m_guySprite = 0;
  sfs::SpriteId m_orbSprite = 0;
  int m_kills = 0;
  double m_fps = 0.0;

  // Procedural state. Each platform is stored as (centerX, centerY, halfX,
  // halfY) so enemies can be spawned on a random platform's surface.
  std::mt19937 m_rng{1337};
  std::vector<glm::vec4> m_platforms;
};
