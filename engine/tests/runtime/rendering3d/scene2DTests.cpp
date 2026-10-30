#include "../../testHarness.h"
#include "mockRenderer3D.h"

#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/pointLightComponent.h>
#include <engine/core/components/spriteComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/particles/decal.h>
#include <engine/core/particles/particleEffect.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering3d/meshLibrary.h>
#include <engine/runtime/rendering3d/modules/decalModule.h>
#include <engine/runtime/rendering3d/modules/particleModule.h>
#include <engine/runtime/rendering3d/modules/spriteModule.h>
#include <engine/runtime/rendering3d/renderSystem3D.h>
#include <engine/runtime/sceneManager/scene.h>

using namespace sfs;
using sfs::testing3d::MockRenderer3D;

namespace
{
// A 2D game's scene: pixels on a plane whose y runs down the screen.
struct Harness
{
  AssetStore store;
  MockRenderer3D renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer3D = &renderer}};
  RenderSystem3D& system = scene.addSystem<RenderSystem3D>(renderer);

  Harness()
  {
    Space2D space;
    space.yDown = true;
    system.setSpace2D(space);
    store.addWhitePixelTexture("white_pixel");
    store.addRadialTexture("white_dot", 32);
  }

  void frame(double seconds = 0.0)
  {
    scene.update(seconds);
    scene.render();
  }
};
} // namespace

int main()
{
  TEST("a flat plane should be the world's, its y flipped if it runs down")
  {
    Space2D flat;
    CHECK(testing::approx(flat.place({3.0f, 4.0f}).y, 4.0f));
    flat.yDown = true;
    CHECK(testing::approx(flat.place({3.0f, 4.0f}).y, -4.0f));
    CHECK(testing::approx(flat.place({3.0f, 4.0f}, 0.0f, 2).z, 2.0f));
  }

  TEST("an isometric grid should draw as 2:1 diamonds, nearer tiles in front")
  {
    Space2D iso;
    iso.projection = Space2D::Projection::Isometric;
    const glm::vec3 origin = iso.place({0.0f, 0.0f});
    const glm::vec3 alongX = iso.place({1.0f, 0.0f}) - origin;
    const glm::vec3 alongY = iso.place({0.0f, 1.0f}) - origin;
    CHECK(testing::approx(alongX.x, 16.0f) && testing::approx(alongX.y, -8.0f));
    CHECK(testing::approx(alongY.x, -16.0f) &&
          testing::approx(alongY.y, -8.0f));
    CHECK(iso.place({1.0f, 1.0f}).z > origin.z);
    // A unit of height lifts it up the screen.
    CHECK(testing::approx(iso.place({0.0f, 0.0f}, 1.0f).y - origin.y, 8.0f));
    // And what's drawn somewhere, at a height, is found again.
    const glm::vec2 found =
        iso.unplace(glm::vec2(iso.place({3.0f, -2.0f}, 4.0f)), 4.0f);
    CHECK(testing::approx(found.x, 3.0f) && testing::approx(found.y, -2.0f));
  }

  TEST("a sprite should be a quad of its frame where the plane puts it")
  {
    Harness h;
    h.system.withModule<SpriteModule>(h.store);
    const SpriteId sprite =
        h.store.addSprite("white_pixel", "dot", SDL_Rect{0, 0, 1, 1});
    Entity e = h.scene.createEntity();
    e.addComponent<SpriteComponent>(sprite, glm::vec2{0.5f, 0.5f});
    e.addComponent<TransformComponent>(
        TransformComponent{glm::vec2{10.0f, 20.0f}, glm::vec2{4.0f, 2.0f}});
    h.frame();

    CHECK(h.renderer.drawn.size() == 1u);
    const MeshInstance& quad = h.renderer.drawn.front();
    CHECK(testing::approx(quad.model[3].x, 10.0f));
    CHECK(testing::approx(quad.model[3].y, -20.0f));
    CHECK(testing::approx(quad.model[0].x, 4.0f));
    CHECK(testing::approx(quad.model[1].y, 2.0f));
    CHECK(testing::approx(quad.uvRect.z, 1.0f));
  }

  TEST("a 2D game's camera and lights should be placed on its plane")
  {
    Harness h;
    Entity camera = h.scene.createEntity();
    camera.addComponent<TransformComponent>(
        TransformComponent{glm::vec2{100.0f, 50.0f}});
    CameraComponent lens;
    lens.orthographic = true;
    camera.addComponent<CameraComponent>(lens);
    Entity lamp = h.scene.createEntity();
    lamp.addComponent<TransformComponent>(
        TransformComponent{glm::vec2{5.0f, 5.0f}});
    lamp.addComponent<PointLightComponent>(glm::vec3(1.0f), 1.0f, 40.0f);
    h.frame();

    CHECK(testing::approx(h.renderer.frame.cameraPosition.x, 100.0f));
    CHECK(testing::approx(h.renderer.frame.cameraPosition.y, -50.0f));
    CHECK(h.renderer.frame.cameraForward.z < -0.99f);
    CHECK(h.renderer.lights.size() == 1u);
    CHECK(testing::approx(h.renderer.lights[0].position.y, -5.0f));
    CHECK(h.renderer.lights[0].position.z >= 10.0f); // in front of the plane
  }

  TEST("a mark should be cut to its surface, and fade as it should")
  {
    Harness h;
    DecalModule& decals = h.system.withModule<DecalModule>(h.store);
    DecalSpawn spawn;
    spawn.worldPos = {0.0f, 0.0f};
    spawn.size = {4.0f, 4.0f};
    spawn.clipMin = {-1.0f, -1.0f};
    spawn.clipMax = {1.0f, 1.0f};
    spawn.color = glm::vec4(1.0f);
    spawn.fadeRate = 1.0f; // gone in a second
    decals.addDecal(spawn);
    h.frame(0.5);

    CHECK(h.renderer.drawn.size() == 1u);
    const MeshInstance& mark = h.renderer.drawn.front();
    CHECK(testing::approx(glm::length(glm::vec3(mark.model[0])), 2.0f));
    CHECK(testing::approx(mark.uvRect.x, 0.25f));
    CHECK(testing::approx(mark.uvRect.z, 0.5f));
    CHECK(testing::approx(mark.tint.a, 0.5f));
    CHECK(h.renderer.batches.front().together);

    h.frame(0.6);
    CHECK(decals.count() == 0u);
  }

  TEST("particles should draw together, glowing ones additively")
  {
    Harness h;
    ParticleModule& particles = h.system.withModule<ParticleModule>(h.store);
    ParticleEffectDesc spark;
    spark.burstCount = 5;
    spark.lifetime = {1.0f, 1.0f};
    spark.blend = BlendMode::Additive;
    particles.registerEffect("spark", spark);
    particles.spawnBurst("spark", {0.0f, 0.0f}, 0.0f);
    h.frame(0.01);

    CHECK(h.renderer.batches.size() == 1u);
    const MockRenderer3D::Batch& batch = h.renderer.batches.front();
    CHECK(batch.together);
    CHECK(batch.instances == 5u);
    const MeshMaterial* look =
        h.system.meshes().materialData(batch.materials.front());
    CHECK(look && look->alphaMode == AlphaMode::Additive && look->unlit);
  }

  return testing::report("scene2DTests");
}
