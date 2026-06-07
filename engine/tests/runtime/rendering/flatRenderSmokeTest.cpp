#include "../../testHarness.h"

#include <engine/core/components/transformComponent.h>
#include <engine/core/particles/particleEffect.h>
#include <engine/core/rendering/projection/flatProjection.h>
#include <engine/runtime/TextRenderer/textRenderer.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/flatRenderContext.h>
#include <engine/runtime/rendering/iQuadRenderer.h>
#include <engine/runtime/rendering/modules/particles.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/systems/flatRenderSystem.h>

#include <vector>

namespace
{
// Records the particle batches submitted; everything else is a no-op, so the
// flat render path runs with no GL context.
class MockRenderer : public sfs::IQuadRenderer
{
public:
  bool initialize() override { return true; }
  void shutdown() override {}
  unsigned int getOrCreateTexture(const std::string&, SDL_Surface*) override
  {
    return 1;
  }
  unsigned int uploadSurfaceTexture(SDL_Surface*) override { return 1; }
  void deleteTexture(unsigned int) override {}
  void submit(const sfs::Quad&) override {}
  void submit(const sfs::TexturedQuad&) override {}
  void submit(const sfs::FreeformQuad&) override {}
  void submit(const sfs::LitQuad&) override { ++litQuads; }
  void submitLitBatch(const sfs::LitQuadBatch&,
                      unsigned int,
                      unsigned int,
                      bool,
                      int) override
  {
  }
  void submitParticleBatch(const sfs::ParticleBatch& batch,
                           unsigned int,
                           sfs::BlendMode,
                           bool) override
  {
    for (const auto& q : batch.quads)
      particleZ.push_back(q.z);
  }
  void drawImmediate(const sfs::TexturedQuad&) override {}
  void begin() override {}
  void flush() override {}
  void drawLineLoop(const glm::vec2*, int, SDL_Color) override {}
  void setViewportSize(int, int) override {}
  void setSurfaceTime(float) override {}
  void setPointLights(const sfs::PointLightSet&) override {}

  int litQuads = 0;
  std::vector<float> particleZ;
};
} // namespace

int main()
{
  sfs::AssetStore store;
  store.addRadialTexture("dot", 8); // CPU surface the particle effect resolves

  MockRenderer mock;
  sfs::TextRenderer text(mock, store); // ctor is GL-free; init() not called

  sfs::Scene scene(0, sfs::SceneServices{store, mock, text});

  auto& render = scene.addSystem<sfs::FlatRenderSystem>(store, mock);
  auto& particles = render.withModule<sfs::Particles<sfs::FlatRenderContext>>();

  sfs::ParticleEffectDesc effect;
  effect.burstCount = 10;
  effect.lifetime = {1.0f, 1.0f};
  effect.size = {4.0f, 4.0f};
  effect.speed = {0.0f, 0.0f};
  effect.texture = "dot";
  effect.space = sfs::SimulationSpace::World;
  particles.registerEffect("t", effect);

  sfs::FlatProjection projection;
  projection.cameraCenter = {0.0f, 0.0f};
  projection.screenCenter = {400.0f, 300.0f};
  projection.zoom = 1.0f;
  render.setProjection(&projection);

  // World position far from the origin: its raw particle sort-key is well
  // outside the clip volume, so a missing clip-depth fix would show here.
  particles.spawnBurst("t", glm::vec2{900.0f, 600.0f}, 0.0f);

  scene.update(0.016); // simulate -> emit the burst
  scene.render();      // FlatRenderSystem submits the particle batch

  TEST("the flat path should submit particles with clip-ranged depth")
  {
    CHECK(!mock.particleZ.empty()); // particles actually reached the renderer
    for (const float z : mock.particleZ)
      CHECK(z >= -1.0f && z <= 1.0f); // clip-ranged, not raw world coords
  }

  return testing::report("flatRenderSmokeTest");
}
