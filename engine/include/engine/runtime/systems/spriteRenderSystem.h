#pragma once

#include "engine/core/ecs/system.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

class AssetStore;
class IQuadRenderer;

/**
 * Flat 2D sprite renderer. Draws every entity that has a SpriteComponent and a
 * TransformComponent as a screen-space textured quad through the core
 * IQuadRenderer, offset by an optional camera pan. It has no dependency on the
 * isometric projection, elevation, or heightmap, so adding it to a scene renders
 * a plain 2D game; the isometric path is a separate render system.
 */
class SpriteRenderSystem : public System
{
public:
  /**
   * @param assetStore   source of sprites and their textures
   * @param quadRenderer core renderer the sprites are drawn through
   */
  SpriteRenderSystem(AssetStore& assetStore, IQuadRenderer& quadRenderer);

  /**
   * Set the camera pan, in world pixels, subtracted from every sprite position.
   *
   * @param offset top-left of the camera in world space
   */
  void setCameraOffset(const glm::vec2& offset);

  SpriteRenderSystem(const SpriteRenderSystem&) = delete;
  SpriteRenderSystem& operator=(const SpriteRenderSystem&) = delete;

protected:
  void create() override;
  void render() override;

private:
  AssetStore& m_assetStore;
  IQuadRenderer& m_quadRenderer;
  glm::vec2 m_cameraOffset{0.0f, 0.0f};
};

} // namespace sfs
