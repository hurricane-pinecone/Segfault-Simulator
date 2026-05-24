#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/ecs.h" // IWYU pragma: keep
#include "engine/rendering/commands/shadowCommands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderProvider.h"

namespace sfs
{

class IsometricSpriteShadowSystem
    : public System,
      public RenderProvider<IsometricRenderContext, SpriteShadowCommand>
{
public:
  IsometricSpriteShadowSystem(AssetStore& assetStore)
      : m_assetStore(assetStore) {};
  IsometricSpriteShadowSystem(IsometricShadowSettings settings,
                              AssetStore& assetStore)
      : m_assetStore(assetStore), m_shadowSettings(settings) {};

  void computeCommands(const IsometricRenderContext& context) override;

  void setSpriteShadowMaxLength(float length);
  void setSpriteShadowAlpha(float alpha);

protected:
  void create() override;

private:
  void constructSpriteShadows(const IsometricRenderContext& context,
                              const Entity& caster);

private:
  AssetStore& m_assetStore;
  IsometricShadowSettings m_shadowSettings;
};

} // namespace sfs
