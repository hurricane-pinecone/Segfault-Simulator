#include "platformerGame.h"

#include "config.h"
#include "scenes/platformerScene.h"

#include "engine/runtime/input/keyboardInput.h"
#include "engine/runtime/rendering/util/flatCamera.h"
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

  // Camera follows the player.
  m_projection = sfs::makeFlatProjection(
      m_scene->cameraTarget(),
      glm::vec2{static_cast<float>(WINDOW_WIDTH),
                static_cast<float>(WINDOW_HEIGHT)},
      1.0f);

  m_scene->getSystem<sfs::FlatRenderSystem>().setProjection(&m_projection);
}

void PlatformerGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
    isRunning = false;
}
