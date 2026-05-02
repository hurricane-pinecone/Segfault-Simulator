#pragma once

#include "tags/playerTag.h"
#include <SDL_keyboard.h>
#include <SDL_stdinc.h>
#include <engine/components/rigidBodyComponent.h>
#include <engine/ecs/component.h>
#include <engine/ecs/system.h>
#include <glm/glm.hpp>

class PlayerInputSystem : public sfs::System
{
public:
  PlayerInputSystem()
  {
    registerComponent<sfs::RigidBodyComponent>();
    registerComponent<PlayerTag>();
  }

  void update(double deltaTime) override
  {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);

    glm::vec2 direction(0.0f);

    if (keys[SDL_SCANCODE_A])
      direction.x -= 1.0f;
    if (keys[SDL_SCANCODE_D])
      direction.x += 1.0f;
    if (keys[SDL_SCANCODE_W])
      direction.y -= 1.0f;
    if (keys[SDL_SCANCODE_S])
      direction.y += 1.0f;

    if (glm::length(direction) > 0.0f)
    {
      direction = glm::normalize(direction);
    }

    for (const auto& entity : getEntities())
    {
      auto& rb = entity.getComponent<sfs::RigidBodyComponent>();
      rb.velocity = direction * baseSpeed;
    }
  }

private:
  float baseSpeed = 200.0f;
};
