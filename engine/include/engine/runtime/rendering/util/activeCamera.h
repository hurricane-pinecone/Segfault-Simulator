#pragma once

#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"

namespace sfs
{

// The scene's camera: its first active one (CameraComponent +
// TransformComponent, in id order), or an invalid entity when there's none.
// RenderSystem draws through it and CameraSystem moves it.
inline Entity findActiveCamera(Registry& registry)
{
  for (const auto& entity :
       registry.view<CameraComponent, TransformComponent>())
    if (entity.getComponent<CameraComponent>().active)
      return entity;
  return {};
}

} // namespace sfs
