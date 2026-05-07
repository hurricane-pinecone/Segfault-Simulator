
#include "sampleGame.h"
#include "engine/TextRenderer/textRenderer.h"
#include "scenes/gameScene.h"
#include <engine/input/keyboardInput.h>
#include <engine/sceneManager/scene.h>
#include <string>

void SampleGame::onSetup()
{
  // TODO: Create actual title screen and refactor
  // sfs::Scene* titleScene = sceneManager.createScene("Title Scene");
  auto gameScene = sceneManager.createScene<GameScene>("Game");

  isRunning = true;
}

void SampleGame::onRender() { sfs::TextRenderer::drawText(20, 20, "Hello"); }

void SampleGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
  {
#ifdef ENGINE_WEB
    return;
#endif
    isRunning = false;
  }

  if (sceneManager.current()->name() == "Title Scene" &&
      input.mouse().mousePressed(sfs::MouseButton::Left))
    sceneManager.load("Game");
}

void SampleGame::onDestroy() {};
