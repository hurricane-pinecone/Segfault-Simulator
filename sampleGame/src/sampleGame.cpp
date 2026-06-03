
#include "sampleGame.h"
#include "config.h"
#include "scenes/gameScene.h"
#include <engine/input/keyboardInput.h>
#include <engine/rendering/isometricGeometryRenderer.h>
#include <engine/rendering/util/isometric/camera.h>
#include <engine/sceneManager/scene.h>
#include <engine/systems/cameraSystem.h>
#include <engine/systems/isometric/isometricRenderSystem.h>
#include <glm/glm/ext/vector_float2.hpp>
#include <memory>
#include <string>

void SampleGame::onSetup()
{
  m_isoConfig = sfs::IsometricProjectionConfig{
      32,
      16,
      8,
      WORLD_SCALE,
      glm::vec2{WINDOW_WIDTH / 2.0f, WINDOW_HEIGHT / 2.0f},
  };

  // TODO: Create actual title screen and refactor
  // sfs::Scene* titleScene = sceneManager.createScene("Title Scene");
  auto gameScene = sceneManager.createScene<GameScene>("Game");

  isRunning = true;
}

void SampleGame::onUpdate(double deltaTime)
{
  sfs::Scene* scene = sceneManager.current();

  if (!scene || !scene->hasSystem<sfs::IsometricRenderSystem>())
    return;

  const sfs::ActiveCamera camera =
      scene->hasSystem<sfs::CameraSystem>()
          ? scene->getSystem<sfs::CameraSystem>().activeCamera()
          : sfs::ActiveCamera{};

  m_isoProjection = sfs::makeProjection(m_isoConfig, camera);

  scene->getSystem<sfs::IsometricRenderSystem>().setProjection(
      &m_isoProjection);
}

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

std::unique_ptr<sfs::IQuadRenderer>
SampleGame::createQuadRenderer(int windowWidth, int windowHeight)
{
  return std::make_unique<sfs::IsometricGeometryRenderer>(windowWidth,
                                                          windowHeight);
}
