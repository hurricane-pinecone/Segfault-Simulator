#pragma once
#include <sfs/sfs.h>

#include "components/Bob.h"

#include <cmath>

// Moves everything with a Bob: floating sparks, turning crystals, the lift.
class BobSystem : public sfs::System
{
protected:
  void create() override
  {
    registerComponent<Bob>();
    registerComponent<sfs::TransformComponent>();
  }

  void update(double deltaTime) override
  {
    for (sfs::Entity& entity : getEntities())
    {
      Bob& bob = entity.getComponent<Bob>();
      auto& transform = entity.getComponent<sfs::TransformComponent>();
      if (bob.clock < 0.0f)
      {
        bob.home = transform.position;
        bob.facing = transform.rotation;
        // Start each one somewhere different along its way.
        bob.clock = static_cast<float>(entity.getId() % 17) * 0.37f;
      }
      bob.clock += static_cast<float>(deltaTime);
      const float wave = std::sin(bob.clock * bob.speed * 6.2831853f);
      transform.position = bob.home + bob.axis * (bob.amplitude * wave);
      transform.rotation =
          glm::angleAxis(bob.spin * bob.clock, glm::vec3(0.0f, 0.0f, 1.0f)) *
          bob.facing;
    }
  }
};
