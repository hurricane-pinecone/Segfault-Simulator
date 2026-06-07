#include "../../../testHarness.h"

#include <engine/core/components/shadowCasterComponent.h>
#include <engine/core/components/spriteComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/ecs.h>
#include <engine/core/rendering/projection/isometricProjection.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/isometricRenderContext.h>
#include <engine/runtime/rendering/modules/spriteShadow.h>
#include <engine/runtime/rendering/util/isometric/isometricLightingUtils.h>

#include <vector>

using namespace sfs;

// These pin the shadow module's GUARD contract (when it refuses to cast), not
// the projected shadow geometry, which depends on lighting math and is verified
// visually rather than asserted here.

namespace
{
IsometricProjection makeProjection()
{
  IsometricProjection p;
  p.tileWidth = 32;
  p.tileHeight = 16;
  p.elevationStep = 8;
  p.worldScale = 1.0f;
  p.zoom = 1.0f;
  return p;
}

Entity addCaster(Registry& reg, SpriteId sprite)
{
  Entity e = reg.createEntity();
  e.addComponent<TransformComponent>(TransformComponent{{0.0f, 0.0f}});
  e.addComponent<SpriteComponent>(SpriteComponent{sprite});
  e.addComponent<ShadowCasterComponent>(ShadowCasterComponent{});
  return e;
}
} // namespace

int main()
{
  TEST("with no light source a caster should cast no shadow")
  {
    Registry reg;
    AssetStore store;
    store.addRadialTexture("tex", 16);
    const SpriteId sprite =
        store.addSprite("tex", "spr", SDL_Rect{0, 0, 16, 16});
    addCaster(reg, sprite);

    SpriteShadow shadows;
    shadows.init(ModuleInit{&reg, &store});
    IsometricProjection proj = makeProjection();
    IsometricRenderContext ctx; // no ambient lighting, no point lights
    ctx.projection = &proj;
    shadows.computeCommands(ctx);

    CHECK(shadows.commands().empty());
  }

  TEST("an unresolvable sprite should cast no shadow")
  {
    Registry reg;
    AssetStore store;
    addCaster(reg, 9999); // no such sprite

    SpriteShadow shadows;
    shadows.init(ModuleInit{&reg, &store});
    IsometricProjection proj = makeProjection();
    IsometricAmbientLighting ambient; // a light exists, but the sprite doesn't
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    ctx.ambientLighting = &ambient;
    shadows.computeCommands(ctx);

    CHECK(shadows.commands().empty());
  }

  TEST("no casters should produce no shadow commands")
  {
    Registry reg;
    AssetStore store;
    SpriteShadow shadows;
    shadows.init(ModuleInit{&reg, &store});
    IsometricProjection proj = makeProjection();
    IsometricAmbientLighting ambient;
    IsometricRenderContext ctx;
    ctx.projection = &proj;
    ctx.ambientLighting = &ambient;
    shadows.computeCommands(ctx);

    CHECK(shadows.commands().empty());
  }

  return testing::report("spriteShadowTests");
}
