#include "../../testHarness.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/pointLight3DComponent.h>
#include <engine/core/components/transform3DComponent.h>
#include <engine/core/rendering/3d/meshPrimitives.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering3d/iRenderer3D.h>
#include <engine/runtime/rendering3d/modules/meshModule.h>
#include <engine/runtime/rendering3d/renderSystem3D.h>
#include <engine/runtime/sceneManager/scene.h>

#include <algorithm>
#include <vector>

using namespace sfs;

namespace
{
// Records what the render system hands the renderer; no GPU is touched.
class MockRenderer3D : public IRenderer3D
{
public:
  MeshHandle createMesh(const MeshData& mesh) override
  {
    if (mesh.empty())
      return {};
    live.push_back(++nextId);
    return MeshHandle{nextId};
  }

  void destroyMesh(MeshHandle mesh) override
  {
    live.erase(std::remove(live.begin(), live.end(), mesh.id), live.end());
  }

  glm::ivec2 viewportSize() const override { return viewport; }
  uint32_t shadowMapResolution() const override { return 1024; }

  void beginFrame(const FrameParams3D& params) override
  {
    frame = params;
    lights.assign(params.pointLights.begin(), params.pointLights.end());
    batches.clear();
    drawn.clear();
    ++frames;
  }

  void drawMesh(const MeshBatch3D& batch) override
  {
    batches.push_back({batch.mesh,
                       batch.instances.size(),
                       batch.shadowInstances.size()});
    drawn.insert(drawn.end(), batch.instances.begin(), batch.instances.end());
  }

  void endFrame() override {}

  struct Batch
  {
    MeshHandle mesh;
    std::size_t instances = 0;
    std::size_t shadowInstances = 0;
  };

  glm::ivec2 viewport{1280, 720};
  uint32_t nextId = 0;
  std::vector<uint32_t> live;
  FrameParams3D frame;
  std::vector<PointLight3D> lights;
  std::vector<Batch> batches;
  std::vector<MeshInstance> drawn; // every lit instance this frame
  int frames = 0;
};

// A scene wired to the mock 3D renderer, with a render system drawing meshes.
// The camera looks along +X at the origin from 20 units away.
struct Harness
{
  AssetStore store;
  MockRenderer3D renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer3D = &renderer}};
  RenderSystem3D& system = scene.addSystem<RenderSystem3D>(renderer);

  Harness()
  {
    system.withModule<MeshModule>();
    system.camera().target = {0.0f, 0.0f, 0.0f};
    system.camera().yaw = 0.0f;
    system.camera().distance = 20.0f;
  }

  Entity spawn(MeshHandle mesh, const glm::vec3& position, bool shadow = true)
  {
    Entity e = scene.createEntity();
    e.addComponent<Transform3DComponent>(Transform3DComponent{position});
    e.addComponent<MeshRendererComponent>(
        MeshRendererComponent{mesh, glm::vec4(1.0f), shadow});
    return e;
  }

  void frame()
  {
    scene.update(0.0); // flush so systems see new entities
    scene.render();
  }
};
} // namespace

int main()
{
  TEST("the scene should expose only the renderers the backend has")
  {
    Harness h;
    CHECK(&h.scene.renderer3D() == &h.renderer);
    CHECK(h.scene.tryRenderer3D() == &h.renderer);
    CHECK(h.scene.tryQuadRenderer() == nullptr);
    CHECK(h.scene.tryTextRenderer() == nullptr);
  }

  TEST("entities sharing a mesh should batch into one draw")
  {
    Harness h;
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    for (int i = 0; i < 5; ++i)
      h.spawn(box, {0.0f, static_cast<float>(i) - 2.0f, 0.0f});

    h.frame();

    CHECK(h.renderer.batches.size() == 1u);
    CHECK(h.renderer.batches[0].mesh == box);
    CHECK(h.renderer.batches[0].instances == 5u);
    CHECK(h.system.stats().drawCalls == 1u);
    CHECK(h.system.stats().instances == 5u);
  }

  TEST("different meshes should draw separately")
  {
    Harness h;
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    const MeshHandle ball = h.system.meshes().add(meshes::sphere(0.5f, 8, 6));
    h.spawn(box, {0, 0, 0});
    h.spawn(ball, {0, 1, 0});
    h.spawn(box, {0, -1, 0});

    h.frame();

    CHECK(h.renderer.batches.size() == 2u);
  }

  TEST("an object behind the camera should not be drawn lit")
  {
    Harness h;
    h.system.setShadowSettings({.enabled = false});
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    h.spawn(box, {-60.0f, 0.0f, 0.0f}); // camera sits at x ~ -16, facing +X

    h.frame();

    CHECK(h.renderer.batches.empty());
    CHECK(h.system.stats().instances == 0u);
  }

  TEST("an off-screen caster near the target should still cast a shadow")
  {
    Harness h;
    h.system.camera().projection = Camera3D::Projection::Orthographic;
    h.system.camera().orthoHeight = 4.0f; // a narrow view
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    h.spawn(box, {0.0f, 15.0f, 0.0f}); // out of view, inside shadow distance

    h.frame();

    CHECK(h.renderer.batches.size() == 1u);
    CHECK(h.renderer.batches[0].instances == 0u);
    CHECK(h.renderer.batches[0].shadowInstances == 1u);
  }

  TEST("non-casters and a set sun should produce no shadow instances")
  {
    Harness h;
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    h.spawn(box, {0, 0, 0}, false);
    h.frame();
    CHECK(h.renderer.batches[0].shadowInstances == 0u);

    Harness night;
    SceneLighting3D lighting;
    lighting.sun.direction = {0.3f, 0.2f, -0.5f}; // below the horizon
    night.system.setLighting(lighting);
    const MeshHandle nightBox = night.system.meshes().add(meshes::box({1, 1, 1}));
    night.spawn(nightBox, {0, 0, 0});
    night.frame();
    CHECK(!night.renderer.frame.shadows.enabled);
    CHECK(night.renderer.batches[0].shadowInstances == 0u);
  }

  TEST("an entity without a valid mesh should be skipped")
  {
    Harness h;
    h.spawn(MeshHandle{}, {0, 0, 0});
    h.spawn(MeshHandle{999}, {0, 0, 0}); // not from this library
    h.frame();
    CHECK(h.renderer.batches.empty());
  }

  TEST("instances should carry the entity's transform and tint")
  {
    Harness h;
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    Entity e = h.spawn(box, {1.0f, 2.0f, 3.0f});
    e.getComponent<MeshRendererComponent>().tint = {1.0f, 0.0f, 0.0f, 1.0f};

    h.frame();

    CHECK(h.renderer.drawn.size() == 1u);
    const MeshInstance& instance = h.renderer.drawn.front();
    CHECK(testing::approx(instance.model[3].x, 1.0f));
    CHECK(testing::approx(instance.model[3].y, 2.0f));
    CHECK(testing::approx(instance.model[3].z, 3.0f));
    CHECK(testing::approx(instance.tint.g, 0.0f));
  }

  TEST("point lights should resolve their offset into world space")
  {
    Harness h;
    Entity lamp = h.scene.createEntity();
    lamp.addComponent<Transform3DComponent>(
        Transform3DComponent{{4.0f, 5.0f, 0.0f}});
    lamp.addComponent<PointLight3DComponent>(
        PointLight3DComponent{{1, 1, 1}, 1.0f, 6.0f, {0.0f, 0.0f, 2.0f}});

    h.frame();

    CHECK(h.renderer.lights.size() == 1u);
    CHECK(testing::approx(h.renderer.lights[0].position.z, 2.0f));
    CHECK(testing::approx(h.renderer.lights[0].radius, 6.0f));
  }

  TEST("too many point lights should keep the ones nearest the target")
  {
    Harness h;
    const auto count = static_cast<int>(kMaxPointLights3D) + 8;
    for (int i = 0; i < count; ++i)
    {
      Entity lamp = h.scene.createEntity();
      lamp.addComponent<Transform3DComponent>(
          Transform3DComponent{{static_cast<float>(i), 0.0f, 0.0f}});
      lamp.addComponent<PointLight3DComponent>();
    }

    h.frame();

    CHECK(h.renderer.lights.size() == kMaxPointLights3D);
    float furthest = 0.0f;
    for (const PointLight3D& light : h.renderer.lights)
      furthest = std::max(furthest, light.position.x);
    CHECK(furthest < static_cast<float>(kMaxPointLights3D));
  }

  TEST("a zero-size viewport should skip the frame")
  {
    Harness h;
    h.renderer.viewport = {0, 0};
    h.frame();
    CHECK(h.renderer.frames == 0);
  }

  TEST("meshes should be freed with the render system")
  {
    MockRenderer3D renderer;
    {
      AssetStore store;
      Scene scene{0,
                  SceneServices{.assetStore = store, .renderer3D = &renderer}};
      auto& system = scene.addSystem<RenderSystem3D>(renderer);
      system.meshes().add(meshes::box({1, 1, 1}));
      system.meshes().add(meshes::plane({1, 1}));
      CHECK(renderer.live.size() == 2u);
    }
    CHECK(renderer.live.empty());
  }

  TEST("an empty mesh should not upload")
  {
    Harness h;
    CHECK(!h.system.meshes().add(MeshData{}).valid());
    CHECK(h.system.meshes().size() == 0u);
  }

  return testing::report("renderSystem3DTests");
}
