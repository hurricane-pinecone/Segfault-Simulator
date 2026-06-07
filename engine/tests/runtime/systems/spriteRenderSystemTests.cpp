#include "../../testHarness.h"

#include <engine/core/components/spriteComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/runtime/TextRenderer/textRenderer.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/iQuadRenderer.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/systems/spriteRenderSystem.h>

#include <vector>

using namespace sfs;

namespace
{
// Records the textured quads the sprite system submits; no GL is touched.
class MockRenderer : public IQuadRenderer
{
public:
  bool initialize() override { return true; }
  void shutdown() override {}
  unsigned int getOrCreateTexture(const std::string&, SDL_Surface*) override
  {
    return 7; // a stable non-zero handle
  }
  unsigned int uploadSurfaceTexture(SDL_Surface*) override { return 7; }
  void deleteTexture(unsigned int) override {}
  void submit(const Quad&) override {}
  void submit(const TexturedQuad& q) override { quads.push_back(q); }
  void submit(const FreeformQuad&) override {}
  void submit(const LitQuad&) override {}
  void submitLitBatch(const LitQuadBatch&,
                      unsigned int,
                      unsigned int,
                      bool,
                      int) override
  {
  }
  void submitParticleBatch(const ParticleBatch&,
                           unsigned int,
                           BlendMode,
                           bool) override
  {
  }
  void drawImmediate(const TexturedQuad&) override {}
  void begin() override {}
  void flush() override {}
  void drawLineLoop(const glm::vec2*, int, SDL_Color) override {}
  void setViewportSize(int, int) override {}
  void setSurfaceTime(float) override {}
  void setPointLights(const PointLightSet&) override {}

  std::vector<TexturedQuad> quads;
};

// A scene wired to a mock renderer, with the sprite system attached. Returns
// the renderer (holds the captured quads) and keeps the scene alive via
// `holder`.
struct Harness
{
  AssetStore store;
  MockRenderer mock;
  TextRenderer text{mock, store};
  Scene scene{0, SceneServices{store, mock, text}};
  SpriteRenderSystem& sprites =
      scene.addSystem<SpriteRenderSystem>(store, mock);

  SpriteId sprite16()
  {
    store.addRadialTexture("tex", 16); // a 16x16 CPU surface
    return store.addSprite("tex", "spr", SDL_Rect{0, 0, 16, 16});
  }
};
} // namespace

int main()
{
  TEST("a sprite should submit a quad placed at its anchored screen position")
  {
    Harness h;
    const SpriteId id = h.sprite16();
    Entity e = h.scene.createEntity();
    e.addComponent<TransformComponent>(TransformComponent{{100.0f, 200.0f}});
    e.addComponent<SpriteComponent>(
        SpriteComponent{id, {0.5f, 1.0f}}); // bottom-centre anchor

    h.scene.update(0.0); // flush so the system matches the entity
    h.scene.render();

    CHECK(h.mock.quads.size() == 1);
    const TexturedQuad& q = h.mock.quads.front();
    CHECK(q.destRect.x == 92);  // 100 - anchorX(0.5*16=8)
    CHECK(q.destRect.y == 184); // 200 - anchorY(1.0*16=16)
    CHECK(q.destRect.w == 16);
    CHECK(q.destRect.h == 16);
    CHECK(q.srcRect.x == 0);
    CHECK(q.srcRect.w == 16);
    CHECK(q.textureWidth == 16);
    CHECK(q.texture == 7);
  }

  TEST("the camera offset should shift the sprite's screen position")
  {
    Harness h;
    const SpriteId id = h.sprite16();
    Entity e = h.scene.createEntity();
    e.addComponent<TransformComponent>(TransformComponent{{100.0f, 200.0f}});
    e.addComponent<SpriteComponent>(SpriteComponent{id, {0.5f, 1.0f}});
    h.sprites.setCameraOffset({10.0f, 20.0f});

    h.scene.update(0.0);
    h.scene.render();

    const TexturedQuad& q = h.mock.quads.front();
    CHECK(q.destRect.x == 82);  // (100-10) - 8
    CHECK(q.destRect.y == 164); // (200-20) - 16
  }

  TEST("transform scale should size the destination quad")
  {
    Harness h;
    const SpriteId id = h.sprite16();
    Entity e = h.scene.createEntity();
    e.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f}, {2.0f, 1.0f}}); // 2x wide
    e.addComponent<SpriteComponent>(SpriteComponent{id, {0.0f, 0.0f}});

    h.scene.update(0.0);
    h.scene.render();

    const TexturedQuad& q = h.mock.quads.front();
    CHECK(q.destRect.w == 32); // 16 * 2
    CHECK(q.destRect.h == 16); // 16 * 1
  }

  TEST("a missing sprite should submit nothing")
  {
    Harness h;
    Entity e = h.scene.createEntity();
    e.addComponent<TransformComponent>(TransformComponent{{0.0f, 0.0f}});
    e.addComponent<SpriteComponent>(SpriteComponent{9999}); // no such sprite

    h.scene.update(0.0);
    h.scene.render();

    CHECK(h.mock.quads.empty());
  }

  return testing::report("spriteRenderSystemTests");
}
