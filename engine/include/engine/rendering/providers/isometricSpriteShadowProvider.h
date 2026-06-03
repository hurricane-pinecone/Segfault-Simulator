#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/ecs/registry.h" // IWYU pragma: keep
#include "engine/rendering/commands/shadowCommands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderProvider.h"

namespace sfs
{

/**
 * Builds projected shadow quads for sprite casters, draping each caster's
 * silhouette over terrain and up walls under the sun and any point lights. A
 * render-helper owned by the isometric render system: it pulls casters from a
 * Registry view and emits textured shadow commands through the RenderProvider
 * interface.
 */
class IsometricSpriteShadowProvider
    : public RenderProvider<IsometricRenderContext, SpriteShadowCommand>
{
public:
  IsometricSpriteShadowProvider() = default;

  /** Set the registry the caster view reads from. */
  void setRegistry(Registry* r) { registry = r; }

  /** Set the asset store the caster sprites/surfaces resolve through. */
  void setAssetStore(AssetStore* a) { m_assetStore = a; }

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
