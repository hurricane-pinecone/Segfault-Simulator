#pragma once

#include "engine/ecs/entity.h"
#include "engine/input/input.h"
#include "engine/sceneManager/scene.h"

class GameScene : public sfs::Scene
{
public:
  using sfs::Scene::Scene;

protected:
  void onInit() override;
  void onProcessInput(const sfs::Input& input) override;

private:
  void loadMap();
  void createPlayer();

private:
  sfs::Entity m_player;
};
