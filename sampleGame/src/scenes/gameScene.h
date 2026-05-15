#pragma once

#include "controllers/sunController.h"
#include "engine/assetStore/assetStore.h"
#include "engine/input/input.h"
#include "engine/sceneManager/scene.h"
#include "glm/glm/ext/vector_float2.hpp"

class GameScene : public sfs::Scene
{
public:
  GameScene(sfs::SceneId id,
            sfs::AssetStore& assetStore,
            const std::string& name)
      : sfs::Scene(id, assetStore, name), m_sunController()
  {
  }

protected:
  void onInit() override;
  void onProcessInput(const sfs::Input& input) override;
  void onUpdate(double deltaTime) override;
  void onPostRender() override;

private:
  void loadMap();
  void createEntities();

private:
  glm::vec2 m_mousePos;
  float m_worldWaveTime;
  SunController m_sunController;
};
