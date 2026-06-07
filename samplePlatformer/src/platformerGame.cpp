#include "platformerGame.h"

#include "config.h"
#include "scenes/platformerScene.h"

#include "engine/core/rendering/projection/flatCamera.h"
#include "engine/runtime/input/keyboardInput.h"
#include "engine/runtime/systems/cameraSystem.h"
#include "engine/runtime/systems/flatRenderSystem.h"
#include "glm/glm/ext/vector_float2.hpp"

void PlatformerGame::onSetup()
{
  m_scene = sceneManager.createScene<PlatformerScene>("Platformer");
  isRunning = true;
}

void PlatformerGame::onUpdate(double /*deltaTime*/)
{
  if (!m_scene || !m_scene->hasSystem<sfs::FlatRenderSystem>())
    return;

  // Camera follows the player through the engine CameraSystem; the active
  // camera resolves the followed position + offset + shake into a world centre.
  const sfs::ActiveCamera camera =
      m_scene->hasSystem<sfs::CameraSystem>()
          ? m_scene->getSystem<sfs::CameraSystem>().activeCamera()
          : sfs::ActiveCamera{};
  const glm::vec2 focus = camera.getCameraPosition();
  m_projection =
      sfs::makeFlatProjection(focus,
                              glm::vec2{static_cast<float>(WINDOW_WIDTH),
                                        static_cast<float>(WINDOW_HEIGHT)},
                              1.0f);

  auto& render = m_scene->getSystem<sfs::FlatRenderSystem>();
  render.setProjection(&m_projection);
  render.setFocus(focus);
}

void PlatformerGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
  {
#ifndef ENGINE_WEB
    isRunning = false; // on web, Esc must not kill the tab's game loop
#endif
  }
}
