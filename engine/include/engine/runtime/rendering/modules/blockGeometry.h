#pragma once

#include "engine/core/ecs/registry.h" // IWYU pragma: keep -- registry->view<T...>()
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/isometricRenderContext.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <string>
#include <unordered_map>

namespace sfs
{

class AssetStore;

// Render module that draws terrain tiles as REAL face geometry -- a top quad
// plus the camera-facing exposed side faces -- instead of billboard sprites.
// Registering it turns block geometry on; leaving it out makes
// IsometricRenderSystem draw tiles as billboards. The two never run together:
// the render system skips tile billboards while this module is registered and
// emits this module's faces in their place.
//
// Side faces carry per-vertex elevation, so a point light at a wall's base
// lights the face from the bottom up. Side faces are split one quad per
// elevation level, so the sprite's side art tiles cleanly down tall cliffs.
//
// Emits one GeometryCommand per (texture, surface effect) bucket each frame.
class BlockGeometry
    : public CommandModule<IsometricRenderContext, GeometryCommand>
{
public:
  void init(const ModuleInit& m) override
  {
    registry = m.registry;
    m_assetStore = m.assetStore;
  }

  bool providesTerrainGeometry() const override { return true; }

  void computeCommands(const IsometricRenderContext& context) override;

private:
  // Texture dimensions (pixels) for srcRect -> normalised UV, cached per frame.
  glm::vec2 textureSize(const std::string& textureId);

  Registry* registry = nullptr;
  AssetStore* m_assetStore = nullptr;
  std::unordered_map<std::string, glm::vec2> m_textureSizeCache;
};

} // namespace sfs
