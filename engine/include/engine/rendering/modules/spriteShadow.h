#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/registry.h" // IWYU pragma: keep
#include "engine/rendering/commands/shadowCommands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/modules/renderModule.h"

namespace sfs
{

/**
 * Render module that builds projected shadow quads for sprite casters, draping
 * each caster's silhouette over terrain and up walls under the sun and any
 * point lights. It pulls casters from a Registry view and emits textured shadow
 * commands.
 */
class SpriteShadow : public CommandModule<SpriteShadowCommand>
{
public:
  SpriteShadow() = default;

  void init(const ModuleInit& m) override
  {
    registry = m.registry;
    m_assetStore = m.assetStore;
  }

  void computeCommands(const IsometricRenderContext& context) override;

  void setSpriteShadowMaxLength(float length);
  void setSpriteShadowAlpha(float alpha);

  IsometricShadowSettings& shadowSettings() { return m_shadowSettings; }

private:
  void constructSpriteShadows(const IsometricRenderContext& context,
                              const Entity& caster);

private:
  Registry* registry = nullptr;
  AssetStore* m_assetStore = nullptr;
  IsometricShadowSettings m_shadowSettings;
};

} // namespace sfs
