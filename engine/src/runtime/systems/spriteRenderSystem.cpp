#include "engine/runtime/systems/spriteRenderSystem.h"

#include "SDL2/SDL_rect.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h" // IWYU pragma: keep -- Entity::getComponent<T> defs
#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering/iQuadRenderer.h"
#include "engine/runtime/rendering/quads.h"
#include "glm/glm/common.hpp"

namespace sfs
{

SpriteRenderSystem::SpriteRenderSystem(AssetStore& assetStore,
                                       IQuadRenderer& quadRenderer)
    : m_assetStore(assetStore), m_quadRenderer(quadRenderer)
{
}

void SpriteRenderSystem::create()
{
  registerComponent<SpriteComponent>();
  registerComponent<TransformComponent>();
}

void SpriteRenderSystem::setCameraOffset(const glm::vec2& offset)
{
  m_cameraOffset = offset;
}

void SpriteRenderSystem::render()
{
  m_quadRenderer.begin();

  for (const auto& entity : getEntities())
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& spriteComponent = entity.getComponent<SpriteComponent>();

    const Sprite* sprite = m_assetStore.getSprite(spriteComponent.spriteId);
    if (!sprite)
    {
      LOG_ERROR("SpriteRenderSystem: sprite not found");
      continue;
    }

    SDL_Surface* surface = m_assetStore.getSurface(sprite->textureId);
    if (!surface)
    {
      LOG_ERROR("SpriteRenderSystem: surface not found");
      continue;
    }

    const int width = static_cast<int>(sprite->srcRect.w * transform.scale.x);
    const int height = static_cast<int>(sprite->srcRect.h * transform.scale.y);

    // Anchor places the sprite image on its world position (e.g. {0.5, 1.0}
    // pins the bottom-centre to the feet).
    const float anchorX = spriteComponent.anchor.x * static_cast<float>(width);
    const float anchorY = spriteComponent.anchor.y * static_cast<float>(height);

    const glm::vec2 screen =
        transform.position - m_cameraOffset + spriteComponent.renderOffset;

    TexturedQuad quad;
    quad.texture = m_quadRenderer.getOrCreateTexture(sprite->textureId, surface);
    quad.srcRect = sprite->srcRect;
    quad.destRect = SDL_Rect{
        static_cast<int>(glm::round(screen.x - anchorX)),
        static_cast<int>(glm::round(screen.y - anchorY)),
        width,
        height,
    };
    quad.textureWidth = surface->w;
    quad.textureHeight = surface->h;

    m_quadRenderer.submit(quad);
  }

  m_quadRenderer.flush();
}

} // namespace sfs
