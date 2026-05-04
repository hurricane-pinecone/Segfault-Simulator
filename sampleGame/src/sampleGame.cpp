
#include "sampleGame.h"
#include "config.h"
#include "engine/input/keyboardInput.h"
#include "engine/sceneManager/scene.h"
#include "scenes/gameScene.h"
#include <SDL_keyboard.h>
#include <engine/components/cameraComponent.h>
#include <engine/mapLoader/mapLoader.h>
#include <engine/systems/cameraSystem.h>
#include <engine/systems/movementSystem.h>
#include <engine/systems/renderSystem.h>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_int2.hpp>
#include <glm/glm.hpp>
#include <string>

void SampleGame::onSetup()
{

  assetStore->addTexture("jungle", ASSET_ROOT + "spriteSheets/jungle.png");
  assetStore->addTexture(
      "spritesheet", ASSET_ROOT + "spriteSheets/tilemap.png");

  // TODO: Create actual title screen and refactor
  // sfs::Scene* titleScene = sceneManager.createScene("Title Scene");
  auto gameScene = sceneManager.createScene<GameScene>("Game");

  isRunning = true;
}

void SampleGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
  {
    isRunning = false;
  }

  if (sceneManager.current()->name() == "Title Scene" &&
      input.mouse().mousePressed(sfs::MouseButton::Left))
    sceneManager.load("Game");
}

void SampleGame::onDestroy() {};
