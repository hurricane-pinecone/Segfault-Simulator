#pragma once

#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/sceneManager/scene.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"

class Player;

class GameScene : public sfs::Scene
{
public:
  GameScene(sfs::SceneId id,
            sfs::AssetStore& assetStore,
            const std::string& name)
      : sfs::Scene(id, assetStore, name)
  {
  }

protected:
  void onInit() override;
  void onProcessInput(const sfs::Input& input) override;
  void onUpdate(double deltaTime) override;
  void onRender() override;
  void onDebugUI() override;

private:
  void loadMap();
  void createEntities();

private:
  glm::vec2 m_mousePos;
  float m_worldWaveTime;
  double m_fps;
  glm::ivec2 m_hoveredTile{0, 0};
  int m_hoveredElevation = 0;
  bool m_hasHoveredTile = false;
  Player* m_player = nullptr;
};
