
#include "InputController.h"
#include "engine/components/rigidBodyComponent.h"
#include "engine/ecs/entity.h"
#include "engine/input/keyboardInput.h"
#include <glm/glm.hpp>

void InputController::processKeyboardInput(const sfs::KeyboardInput& input,
                                           sfs::Entity& player)
{
  glm::vec2 direction(0.0f);

  if (input.keyHeld(sfs::Key::A))
    direction.x -= 1.0f;
  if (input.keyHeld(sfs::Key::D))
    direction.x += 1.0f;
  if (input.keyHeld(sfs::Key::W))
    direction.y -= 1.0f;
  if (input.keyHeld(sfs::Key::S))
    direction.y += 1.0f;

  if (glm::length(direction) > 0.0f)
  {
    direction = glm::normalize(direction);
  }

  auto& rb = player.getComponent<sfs::RigidBodyComponent>();

  rb.velocity = direction * 200.0f;
}
