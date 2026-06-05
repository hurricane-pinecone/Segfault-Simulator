#pragma once

#include "engine/runtime/assetStore/sprite.h"
#include "engine/runtime/sceneManager/scene.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <random>
#include <vector>

class Player;

// The platformer scene: builds the level (player, platforms, a lit torch that
// emits sparks), runs the local physics system, and renders through the engine's
// flat 2D path (sfs::FlatRenderSystem). No isometric/heightfield types anywhere.
class PlatformerScene : public sfs::Scene
{
public:
  using sfs::Scene::Scene; // inherit (SceneId, SceneServices[, name]) ctors

  // World position the camera should centre on (the player).
  glm::vec2 cameraTarget() const;

protected:
  void onInit() override;
  void onUpdate(double deltaTime) override;
  void onRender() override;

private:
  void createEntities();
  void generatePlatforms(); // procedural ground + floating platforms
  void createPlatform(const glm::vec2& center, const glm::vec2& size);
  void createTorch(const glm::vec2& pos, const glm::vec3& color);
  void createEnemy(const glm::vec2& center);
  void spawnRandomEnemy(); // on a random generated platform (testing)

  // A short-lived point light (muzzle/death flash).
  void spawnFlash(const glm::vec2& pos, const glm::vec3& color, float radius,
                  float time);

  Player* m_player = nullptr;
  sfs::SpriteId m_platformSprite = 0;
  sfs::SpriteId m_guySprite = 0;
  int m_kills = 0;
  double m_fps = 0.0;

  // Procedural state. Each platform is stored as (centerX, centerY, halfX,
  // halfY) so enemies can be spawned on a random platform's surface.
  std::mt19937 m_rng{1337};
  std::vector<glm::vec4> m_platforms;

  // Screen shake (applied to the camera target).
  float m_shake = 0.0f;
  double m_shakeTime = 0.0;
  glm::vec2 m_shakeOffset{0.0f, 0.0f};
};
