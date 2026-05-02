#pragma once

#include "engine/systems/cameraSystem.h"
#include <SDL_rect.h>
#include <SDL_render.h>
#include <engine/assetStore/assetStore.h>
#include <engine/components/spriteComponent.h>
#include <engine/components/transformComponent.h>
#include <engine/ecs/system.h>
#include <engine/logger/logger.h>
#include <glm/ext/vector_float2.hpp>

namespace sfs
{

class RenderSystem : public System
{
public:
  RenderSystem(AssetStore& assetStore, int windowWidth, int windowHeight)
      : assetStore(assetStore), windowWidth(windowWidth),
        windowHeight(windowHeight)
  {
    registerComponent<SpriteComponent>();
    registerComponent<TransformComponent>();
  };

  void render(SDL_Renderer& renderer)
  {
    glm::vec2 cameraPosition{0.0f, 0.0f};

    if (registry->hasSystem<CameraSystem>())
    {
      const auto& camera = registry->getSystem<CameraSystem>().getEntities();

      if (!camera.empty())
      {
        const auto& cameraTransform =
            camera[0].getComponent<TransformComponent>();

        cameraPosition = cameraTransform.position;
      }
    }

    for (const auto& entity : getEntities())
    {
      const auto& transform = entity.getComponent<TransformComponent>();
      const auto& spriteComponent = entity.getComponent<SpriteComponent>();

      const auto sprite = assetStore.getSprite(spriteComponent.spriteId);

      if (!sprite)
      {
        LOG_ERROR("Attempted to render NULL sprite");
        continue;
      }

      SDL_Texture* texture = assetStore.getTexture(sprite->textureId);

      if (!texture)
      {
        LOG_ERROR("NULL texture included in render loop");
        continue;
      }

      glm::vec2 screenCenter{static_cast<float>(windowWidth) / 2.0f,
                             static_cast<float>(windowHeight) / 2.0f};
      glm::vec2 screenPosition =
          transform.position - cameraPosition + screenCenter;

      SDL_Rect dest = {static_cast<int>(screenPosition.x),
                       static_cast<int>(screenPosition.y),
                       static_cast<int>(sprite->width * transform.scale.x),
                       static_cast<int>(sprite->height * transform.scale.y)};

      SDL_Rect* src = nullptr;
      SDL_Rect srcRect;
      if (sprite->positionInSheet)
      {
        // positionInSheet.x => Col
        // positionInSheet.y => Row
        // positionInSheet.z => Gap
        srcRect = SDL_Rect{
            static_cast<int>(sprite->positionInSheet->x *
                             (sprite->width + sprite->positionInSheet->gap)),
            static_cast<int>(sprite->positionInSheet->y *
                             (sprite->height + sprite->positionInSheet->gap)),
            static_cast<int>(sprite->width),
            static_cast<int>(sprite->height)};
        src = &srcRect;
      }

      SDL_RenderCopyEx(&renderer,
                       texture,
                       src,
                       &dest,
                       transform.rotation,
                       NULL,
                       SDL_FLIP_NONE);
    }
  };

  RenderSystem(const RenderSystem&) = delete;
  RenderSystem& operator=(const RenderSystem&) = delete;

private:
  AssetStore& assetStore;
  int windowWidth;
  int windowHeight;
};

} // namespace sfs
