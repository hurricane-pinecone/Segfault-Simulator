#pragma once

#include "engine/systems/cameraSystem.h"
#include "engine/systems/isometricRenderSystem.h"
#include <SDL_rect.h>
#include <SDL_render.h>
#include <engine/assetStore/assetStore.h>
#include <engine/components/spriteComponent.h>
#include <engine/components/transformComponent.h>
#include <engine/ecs/system.h>
#include <engine/logger/logger.h>
#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

class RenderSystem : public System
{
public:
  //   RenderSystem(AssetStore& assetStore, int windowWidth, int windowHeight)
  //       : assetStore(assetStore), windowWidth(windowWidth),
  //         windowHeight(windowHeight)
  //   {
  //     registerComponent<SpriteComponent>();
  //     registerComponent<TransformComponent>();
  //   };
  //
  //   void render(SDL_Renderer& renderer)
  //   {
  //     glm::vec2 cameraPosition{0.0f, 0.0f};
  //
  //     if (registry->hasSystem<CameraSystem>())
  //     {
  //       const auto& camera =
  //       registry->getSystem<CameraSystem>().getEntities();
  //
  //       if (!camera.empty())
  //       {
  //         const auto& cameraTransform =
  //             camera[0].getComponent<TransformComponent>();
  //
  //         cameraPosition = cameraTransform.position;
  //       }
  //     }
  //
  //     for (const auto& entity : getEntities())
  //     {
  //       if (entity.hasComponent<IsometricTile>())
  //         continue;
  //       const auto& transform = entity.getComponent<TransformComponent>();
  //       const auto& spriteComponent = entity.getComponent<SpriteComponent>();
  //
  //       const auto sprite = assetStore.getSprite(spriteComponent.spriteId);
  //
  //       if (!sprite)
  //       {
  //         LOG_ERROR("Attempted to render NULL sprite");
  //         continue;
  //       }
  //
  //       SDL_Texture* texture = assetStore.getTexture(sprite->textureId);
  //
  //       if (!texture)
  //       {
  //         LOG_ERROR("NULL texture included in render loop");
  //         continue;
  //       }
  //
  //       glm::vec2 screenCenter{static_cast<float>(windowWidth) / 2.0f,
  //                              static_cast<float>(windowHeight) / 2.0f};
  //       glm::vec2 screenPosition =
  //           transform.position - cameraPosition + screenCenter;
  //
  //       int width = static_cast<int>(sprite->srcRect.w * transform.scale.x);
  //
  //       int height = static_cast<int>(sprite->srcRect.h * transform.scale.y);
  //
  //       SDL_Rect dest = {static_cast<int>(screenPosition.x - width / 2.0f),
  //                        static_cast<int>(screenPosition.y - height / 2.0f),
  //                        width,
  //                        height};
  //
  //       SDL_RenderCopyEx(&renderer,
  //                        texture,
  //                        &sprite->srcRect,
  //                        &dest,
  //                        transform.rotation,
  //                        NULL,
  //                        SDL_FLIP_NONE);
  //     }
  //   };
  //
  //   RenderSystem(const RenderSystem&) = delete;
  //   RenderSystem& operator=(const RenderSystem&) = delete;
  //
  // private:
  //   AssetStore& assetStore;
  //   int windowWidth;
  //   int windowHeight;
};

} // namespace sfs
