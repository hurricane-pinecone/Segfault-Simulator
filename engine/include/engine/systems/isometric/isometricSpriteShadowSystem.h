#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/system.h"
#include "engine/renderers/commands/shadowCommands.h"
#include "engine/renderers/isometricRenderContext.h"
#include "engine/renderers/renderProvider.h"
#include "engine/utils/isometricLightingUtils.h"

namespace sfs
{

class IsometricSpriteShadowSystem
    : public System,
      public RenderProvider<IsometricRenderContext, SpriteShadowCommand>
{
public:
  IsometricSpriteShadowSystem(AssetStore& assetStore);
  IsometricSpriteShadowSystem(
      IsometricShadowSettings settings,
      AssetStore& assetStore,
      const IsometricAmbientLighting* ambient = nullptr);

  void computeCommands(const IsometricRenderContext& context) override;

  void setAmbientLighting(const IsometricAmbientLighting* ambient);

  void setSpriteShadowMaxLength(float length);
  void setSpriteShadowAlpha(float alpha);

private:
  void constructSpriteShadows(const IsometricRenderContext& context,
                              const Entity& caster);

private:
  AssetStore& m_assetStore;
  IsometricShadowSettings m_shadowSettings;
  const IsometricAmbientLighting* m_ambientLighting = nullptr;
};

} // namespace sfs
