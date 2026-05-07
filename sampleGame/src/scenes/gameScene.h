#pragma once

#include "engine/ecs/entity.h"
#include "engine/input/input.h"
#include "engine/sceneManager/scene.h"
#include "glm/glm/ext/vector_float2.hpp"

class GameScene : public sfs::Scene
{
public:
  using sfs::Scene::Scene;

protected:
  void onInit() override;
  void onProcessInput(const sfs::Input& input) override;
  void onPostRender() override;

private:
  void loadMap();
  void createEntities();

private:
  sfs::Entity m_player;
  glm::vec2 m_mousePos;
};
