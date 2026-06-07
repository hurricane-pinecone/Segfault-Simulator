#pragma once

#include "engine/core/ecs/registry.h" // IWYU pragma: keep
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/commands/shadowCommands.h"
#include "engine/runtime/rendering/isometricRenderContext.h"
#include "engine/runtime/rendering/modules/renderModule.h"

namespace sfs
{

/**
 * Render module that builds projected shadow quads for sprite casters, draping
 * each caster's silhouette over terrain and up walls under the sun and any
 * point lights. It pulls casters from a Registry view and emits textured shadow
 * commands.
 */
class SpriteShadow
    : public CommandModule<IsometricRenderContext, SpriteShadowCommand>
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

  std::vector<ModuleSetting> settings(const IsometricRenderContext&) override
  {
    // Actor shadows are draped billboards in both render styles (actors aren't
    // in the heightmap), so they always apply.
    return {
        settings::floatRange(
            "Sprite shadow length",
            0.0f,
            5.0f,
            [this] { return m_shadowSettings.spriteShadowMaxLength; },
            [this](float v) { m_shadowSettings.spriteShadowMaxLength = v; }),
        settings::floatRange(
            "Sprite alpha",
            0.0f,
            1.0f,
            [this] { return m_shadowSettings.spriteShadowAlpha; },
            [this](float v) { m_shadowSettings.spriteShadowAlpha = v; }),
    };
  }

private:
  void constructSpriteShadows(const IsometricRenderContext& context,
                              const Entity& caster);

private:
  Registry* registry = nullptr;
  AssetStore* m_assetStore = nullptr;
  IsometricShadowSettings m_shadowSettings;
};

} // namespace sfs
