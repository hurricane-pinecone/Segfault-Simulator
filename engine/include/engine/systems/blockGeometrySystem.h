#pragma once

#include "engine/ecs/system.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderProvider.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <string>
#include <unordered_map>

namespace sfs
{

class AssetStore;

// Opt-in extension: renders terrain tiles as REAL face geometry -- a top quad
// plus the camera-facing exposed side faces -- instead of billboard sprites. Add
// it to a scene to turn block geometry on; leave it out and IsometricRenderSystem
// draws tiles as billboards exactly as before. The two never run together: the
// render system skips tile billboards whenever this system is present + enabled,
// and pulls this provider's faces in their place.
//
// Side faces carry per-vertex elevation, so a point light at a wall's base lights
// the face from the bottom up -- the billboard lit the whole block at its top
// elevation, so light never "rose" up a wall. Side faces are also split one quad
// per elevation level, so the sprite's side art tiles cleanly down tall cliffs.
//
// Emits one GeometryCommand per (texture, surface effect) bucket each frame.
class BlockGeometrySystem
    : public System,
      public RenderProvider<IsometricRenderContext, GeometryCommand>
{
public:
  explicit BlockGeometrySystem(AssetStore& assetStore);

  void computeCommands(const IsometricRenderContext& context) override;

protected:
  void create() override;

private:
  // Texture dimensions (pixels) for srcRect -> normalised UV, cached per frame.
  glm::vec2 textureSize(const std::string& textureId);

  AssetStore& m_assetStore;
  std::unordered_map<std::string, glm::vec2> m_textureSizeCache;
};

} // namespace sfs
