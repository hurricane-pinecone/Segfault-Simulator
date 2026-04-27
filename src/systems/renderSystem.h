#pragma once

#include "SDL_error.h"
#include "SDL_image.h"
#include "SDL_rect.h"
#include "SDL_render.h"
#include "SDL_surface.h"
#include "components/spriteComponent.h"
#include "components/transformComponent.h"
#include "ecs/system.h"
#include "logger/logger.h"
#include <unordered_map>

class RenderSystem : public System
{
public:
  RenderSystem()
  {
    registerComponent<SpriteComponent>();
    registerComponent<TransformComponent>();
  };

  ~RenderSystem() override
  {
    for (auto& [path, texture] : textures)
    {
      SDL_DestroyTexture(texture);
    }
  }

  void render(SDL_Renderer* renderer)
  {
    for (auto entity : getEntities())
    {
      const auto transform = entity.getComponent<TransformComponent>();
      const auto sprite = entity.getComponent<SpriteComponent>();

      SDL_Texture* texture = getTexture(renderer, "./assets/" + sprite.path);

      if (!texture)
      {
        LOG_ERROR("NULL texture included in render loop");
        continue;
      }

      SDL_Rect dest = {static_cast<int>(transform.position.x),
                       static_cast<int>(transform.position.y),
                       sprite.width,
                       sprite.height};

      SDL_RenderCopy(renderer, texture, nullptr, &dest);
    }
    SDL_RenderPresent(renderer);
  };

private:
  std::unordered_map<std::string, SDL_Texture*> textures;

  SDL_Texture* getTexture(SDL_Renderer* renderer, const std::string& path)
  {
    if (textures.count(path))
    {
      return textures[path];
    }

    SDL_Surface* surface = IMG_Load(path.c_str());
    if (!surface)
    {
      LOG_ERROR("Error loading image at path: " + path +
                ", SDL ERR: " + std::string(IMG_GetError()));
      return nullptr;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);

    if (!texture)
    {
      LOG_ERROR(std::string("Failed to create texture ") + SDL_GetError());
      return nullptr;
    }

    textures[path] = texture;
    return texture;
  }
};
