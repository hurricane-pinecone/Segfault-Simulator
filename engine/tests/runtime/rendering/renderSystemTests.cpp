#include "../../testHarness.h"
#include "mockRenderer.h"

#include <engine/core/components/boxCollider3DComponent.h>
#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/cameraFollowComponent.h>
#include <engine/core/components/grassBenderComponent.h>
#include <engine/core/components/grassComponent.h>
#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/pointLightComponent.h>
#include <engine/core/components/rigidBody3DComponent.h>
#include <engine/core/components/sphereCollider3DComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/rendering/materialGraphCompiler.h>
#include <engine/core/rendering/meshPrimitives.h>
#include <engine/core/rendering/terrain.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/input/input.h>
#include <engine/runtime/rendering/iRenderer.h>
#include <engine/runtime/rendering/meshLibrary.h>
#include <engine/runtime/rendering/modules/grassModule.h>
#include <engine/runtime/rendering/modules/meshModule.h>
#include <engine/runtime/rendering/modules/uiModule.h>
#include <engine/runtime/rendering/renderSystem.h>
#include <engine/runtime/rendering/visualStyles.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/ui/layer.h>

#include <algorithm>
#include <vector>

using namespace sfs;
using sfs::testing3d::MockRenderer;

namespace
{
// A material from a graph, as the asset store makes one.
MeshMaterial graphMaterial(const materialGraph::Graph& graph)
{
  const materialGraph::CompiledGraph compiled = materialGraph::compile(graph);
  MeshMaterial material;
  material.shader = compiled.shader;
  material.shaderParameters = compiled.parameters;
  material.shaderTextures.resize(compiled.textures.size());
  return material;
}

// A graph material glowing `emission` all over (black otherwise).
MeshMaterial glowMaterial(glm::vec3 emission)
{
  materialGraph::Graph graph;
  const auto out = graph.add(std::string(materialGraph::kOutputType), {});
  graph.find(out)->inputs[materialGraph::kBaseColor] = glm::vec4(0.0f);
  graph.find(out)->inputs[materialGraph::kEmission] = glm::vec4(emission, 1.0f);
  return graphMaterial(graph);
}

// A scene wired to the mock renderer, with a render system drawing meshes.
// The camera looks along +X at the origin from 20 units away.
struct Harness
{
  AssetStore store;
  MockRenderer renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
  RenderSystem& system = scene.addSystem<RenderSystem>(renderer);

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
    e.addComponent<TransformComponent>(TransformComponent{position});
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
  TEST("the scene should expose the backend's renderer")
  {
    Harness h;
    CHECK(&h.scene.renderer() == &h.renderer);
    CHECK(h.scene.tryRenderer() == &h.renderer);
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
    h.system.camera().projection = Camera::Projection::Orthographic;
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
    SceneLighting lighting;
    lighting.sun.direction = {0.3f, 0.2f, -0.5f}; // below the horizon
    night.system.setLighting(lighting);
    const MeshHandle nightBox =
        night.system.meshes().add(meshes::box({1, 1, 1}));
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
    lamp.addComponent<TransformComponent>(
        TransformComponent{{4.0f, 5.0f, 0.0f}});
    lamp.addComponent<PointLightComponent>(
        PointLightComponent{{1, 1, 1}, 1.0f, 6.0f, {0.0f, 0.0f, 2.0f}});

    h.frame();

    CHECK(h.renderer.lights.size() == 1u);
    CHECK(testing::approx(h.renderer.lights[0].position.z, 2.0f));
    CHECK(testing::approx(h.renderer.lights[0].radius, 6.0f));
  }

  TEST("children should draw and light where their parents put them")
  {
    Harness h;
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    Entity cart = h.scene.createEntity();
    cart.addComponent<TransformComponent>(
        TransformComponent{{2.0f, 0.0f, 0.0f}});
    Entity crate = h.spawn(box, {0.0f, 1.0f, 0.0f});
    hierarchy::setParent(crate, cart);
    Entity lantern = h.scene.createEntity();
    lantern.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f, 1.0f}});
    lantern.addComponent<PointLightComponent>();
    hierarchy::setParent(lantern, crate);

    h.frame();

    CHECK(h.renderer.drawn.size() == 1u);
    CHECK(testing::approx(h.renderer.drawn.front().model[3].x, 2.0f));
    CHECK(testing::approx(h.renderer.drawn.front().model[3].y, 1.0f));
    CHECK(h.renderer.lights.size() == 1u);
    CHECK(testing::approx(h.renderer.lights[0].position.x, 2.0f));
    CHECK(testing::approx(h.renderer.lights[0].position.z, 1.0f));
  }

  TEST("the scene should render through its active camera entity")
  {
    Harness h;
    Entity off = h.scene.createEntity();
    off.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f, 50.0f}});
    off.addComponent<CameraComponent>().getComponent<CameraComponent>().active =
        false;

    Entity rig = h.scene.createEntity();
    rig.addComponent<TransformComponent>(
        TransformComponent{{-5.0f, 2.0f, 3.0f}});
    Entity eye = h.scene.createEntity();
    TransformComponent eyeTransform{{0.0f, 0.0f, 1.0f}};
    eyeTransform.setYaw(1.5707964f); // facing +Y
    eye.addComponent<TransformComponent>(eyeTransform);
    CameraComponent lens;
    lens.orthographic = true;
    lens.orthoHeight = 9.0f;
    lens.focusDistance = 4.0f;
    eye.addComponent<CameraComponent>(lens);
    hierarchy::setParent(eye, rig);

    h.frame();

    CHECK(h.system.sceneCamera() == eye);
    const Camera& camera = h.system.camera();
    CHECK(testing::approx(camera.position().x, -5.0f));
    CHECK(testing::approx(camera.position().y, 2.0f));
    CHECK(testing::approx(camera.position().z, 4.0f));
    CHECK(testing::approx(camera.target.y, 6.0f)); // 4 ahead along +Y
    CHECK(camera.projection == Camera::Projection::Orthographic);
    CHECK(testing::approx(camera.orthoHeight, 9.0f));
  }

  TEST("the scene camera's zoom and its follow's shake should reach the view")
  {
    Harness h;
    Entity eye = h.scene.createEntity();
    eye.addComponent<TransformComponent>(
        TransformComponent{{0.0f, 0.0f, 1.0f}}); // facing +X
    CameraComponent lens;
    lens.orthographic = true;
    lens.orthoHeight = 10.0f;
    lens.focusDistance = 4.0f;
    lens.zoom = 2.0f;
    eye.addComponent<CameraComponent>(lens);
    CameraFollowComponent follow;
    follow.shakeMaxOffset = 1.0f;
    follow.shake(1.0f, 1.0f);
    follow.updateShake(0.1f);
    eye.addComponent<CameraFollowComponent>(follow);

    h.frame();

    const Camera& camera = h.system.camera();
    CHECK(testing::approx(camera.orthoHeight, 5.0f)); // twice as big
    // Shaken across the view: sideways is -Y here, up is +Z.
    const glm::vec2 shake = follow.shakeOffset();
    CHECK(std::abs(shake.x) > 0.0f || std::abs(shake.y) > 0.0f);
    CHECK(testing::approx(camera.target.x, 4.0f));
    CHECK(testing::approx(camera.target.y, -shake.x));
    CHECK(testing::approx(camera.target.z, 1.0f + shake.y));
  }

  TEST("not following the scene camera should leave camera() to its driver")
  {
    Harness h;
    Entity eye = h.scene.createEntity();
    eye.addComponent<TransformComponent>(
        TransformComponent{{40.0f, 0.0f, 0.0f}});
    eye.addComponent<CameraComponent>();
    h.system.setFollowSceneCamera(false);

    h.frame();

    CHECK(testing::approx(h.system.camera().target.x, 0.0f));
    CHECK(testing::approx(h.system.camera().distance, 20.0f));
  }

  TEST("screenRay should go through the view's centre pixel to the target")
  {
    Harness h;
    h.frame();
    const glm::ivec2 size = h.renderer.viewportSize();
    const Ray ray = h.system.screenRay(glm::vec2(size) * 0.5f);
    const glm::vec3 toTarget =
        glm::normalize(h.system.camera().target - ray.origin);
    CHECK(testing::approx(glm::dot(ray.direction, toTarget), 1.0f, 1e-3f));
  }

  TEST("screenRay should use the size the scene was drawn at, not a later one")
  {
    Harness h;
    h.frame();
    const glm::vec2 drawn = glm::vec2(h.renderer.viewportSize());
    const Ray before = h.system.screenRay(drawn * 0.5f);
    h.renderer.viewport =
        h.renderer.viewport * 3; // e.g. the window, between frames
    const Ray after = h.system.screenRay(drawn * 0.5f);
    CHECK(testing::approx(
        glm::dot(before.direction, after.direction), 1.0f, 1e-5f));
  }

  TEST("too many point lights should keep the ones nearest the target")
  {
    Harness h;
    const auto count = static_cast<int>(kMaxPointLights) + 8;
    for (int i = 0; i < count; ++i)
    {
      Entity lamp = h.scene.createEntity();
      lamp.addComponent<TransformComponent>(
          TransformComponent{{static_cast<float>(i), 0.0f, 0.0f}});
      lamp.addComponent<PointLightComponent>();
    }

    h.frame();

    CHECK(h.renderer.lights.size() == kMaxPointLights);
    float furthest = 0.0f;
    for (const PointLight& light : h.renderer.lights)
      furthest = std::max(furthest, light.position.x);
    CHECK(furthest < static_cast<float>(kMaxPointLights));
  }

  TEST("a glowing material should light what's around it, through a light "
       "where it glows")
  {
    Harness h;
    const MeshHandle sphere =
        h.system.meshes().add(meshes::sphere(0.5f, 24, 16, glm::vec3(1.0f)));
    h.system.meshes().setMaterial(
        "glow", glowMaterial(glm::vec3(4.0f, 2.0f, 0.0f)));
    Entity lamp = h.spawn(sphere, {2.0f, 3.0f, 0.0f});
    lamp.getComponent<MeshRendererComponent>().materials = {"glow"};

    h.frame();

    CHECK(h.renderer.lights.size() == 1u);
    CHECK(h.system.stats().emissionLights == 1u);
    if (h.renderer.lights.size() == 1u)
    {
      const PointLight& light = h.renderer.lights[0];
      CHECK(glm::length(light.position - glm::vec3(2.0f, 3.0f, 0.5f)) < 0.05f);
      CHECK(testing::approx(light.color.g, 0.5f, 1e-3f));
      CHECK(testing::approx(light.intensity, 4.0f, 1e-3f));
    }

    // Twice the size: four times the glowing area, so it reaches twice as far.
    const float radius =
        h.renderer.lights.empty() ? 0.0f : h.renderer.lights[0].radius;
    lamp.getComponent<TransformComponent>().scale = glm::vec3(2.0f);
    h.frame();
    CHECK(h.renderer.lights.size() == 1u &&
          testing::approx(h.renderer.lights[0].radius, radius * 2.0f, 1e-2f));

    // Half see-through: half the light.
    lamp.getComponent<MeshRendererComponent>().tint.a = 0.5f;
    h.frame();
    CHECK(h.renderer.lights.size() == 1u &&
          testing::approx(h.renderer.lights[0].intensity, 2.0f, 1e-3f));
  }

  TEST(
      "only glowing, light-giving objects should, while emission lights are on")
  {
    Harness h;
    const MeshHandle sphere =
        h.system.meshes().add(meshes::sphere(0.5f, 24, 16, glm::vec3(1.0f)));
    h.system.meshes().setMaterial("glow", glowMaterial(glm::vec3(1.0f)));
    h.system.meshes().setMaterial("dark", glowMaterial(glm::vec3(0.0f)));
    MeshMaterial standard;
    standard.baseColor = glm::vec4(5.0f);
    h.system.meshes().setMaterial("standard", standard);

    h.spawn(sphere, {0.0f, 0.0f, 0.0f})
        .getComponent<MeshRendererComponent>()
        .materials = {"dark"};
    h.spawn(sphere, {0.0f, 2.0f, 0.0f})
        .getComponent<MeshRendererComponent>()
        .materials = {"standard"};
    Entity off = h.spawn(sphere, {0.0f, 4.0f, 0.0f});
    off.getComponent<MeshRendererComponent>().materials = {"glow"};
    off.getComponent<MeshRendererComponent>().emitsLight = false;
    h.spawn(sphere, {0.0f, 6.0f, 0.0f}); // its own (standard) material
    h.frame();
    CHECK(h.renderer.lights.empty());

    Entity glowing = h.spawn(sphere, {0.0f, 8.0f, 0.0f});
    glowing.getComponent<MeshRendererComponent>().materials = {"glow"};
    h.frame();
    CHECK(h.renderer.lights.size() == 1u);

    // Its own strength and range: twice as bright, half as far.
    const PointLight normal =
        h.renderer.lights.empty() ? PointLight{} : h.renderer.lights[0];
    auto& renderer = glowing.getComponent<MeshRendererComponent>();
    renderer.lightStrength = 2.0f;
    renderer.lightRange = 0.5f;
    h.frame();
    CHECK(h.renderer.lights.size() == 1u &&
          testing::approx(
              h.renderer.lights[0].intensity, normal.intensity * 2.0f) &&
          testing::approx(h.renderer.lights[0].radius, normal.radius * 0.5f));
    renderer.lightStrength = 0.0f;
    h.frame();
    CHECK(h.renderer.lights.empty());
  }

  TEST("bloom should be the scene camera's, or the system's without one")
  {
    Harness h;
    h.system.setBloomSettings({.enabled = false});
    h.frame();
    CHECK(!h.renderer.frame.bloom.enabled);

    Entity camera = h.scene.createEntity();
    camera.addComponent<TransformComponent>();
    CameraComponent lens;
    lens.bloomIntensity = 1.7f;
    lens.bloomThreshold = 2.5f;
    camera.addComponent<CameraComponent>(lens);
    h.system.setFollowSceneCamera(false); // as the editor, looking around
    h.frame();
    CHECK(h.renderer.frame.bloom.enabled);
    CHECK(testing::approx(h.renderer.frame.bloom.intensity, 1.7f));
    CHECK(testing::approx(h.renderer.frame.bloom.threshold, 2.5f));
  }

  TEST("many objects sharing a glowing material should each light their own "
       "place")
  {
    Harness h;
    const MeshHandle sphere =
        h.system.meshes().add(meshes::sphere(0.5f, 24, 16, glm::vec3(1.0f)));
    h.system.meshes().setMaterial("glow", glowMaterial(glm::vec3(2.0f)));
    for (int i = 0; i < 300; ++i)
      h.spawn(sphere,
              {static_cast<float>(i % 20) * 3.0f,
               static_cast<float>(i / 20) * 3.0f,
               0.0f})
          .getComponent<MeshRendererComponent>()
          .materials = {"glow"};
    h.frame();
    CHECK(h.renderer.lights.size() == 300u);
    bool eachAtOne = true;
    for (const PointLight& light : h.renderer.lights)
    {
      const glm::vec2 grid = glm::vec2(light.position) / 3.0f;
      eachAtOne = eachAtOne && glm::length(grid - glm::round(grid)) < 0.02f;
    }
    CHECK(eachAtOne);
  }

  TEST("glow that depends on where or when should be worked out there and then")
  {
    Harness h;
    const MeshHandle sphere =
        h.system.meshes().add(meshes::sphere(0.5f, 24, 16, glm::vec3(1.0f)));

    // Emission = the world position (clamped at 0 on the GPU and here): only
    // the object out on the positive side glows.
    {
      materialGraph::Graph g;
      const auto out = g.add(std::string(materialGraph::kOutputType), {});
      const auto position = g.add("position", {});
      g.connect(position, 0, out, materialGraph::kEmission);
      h.system.meshes().setMaterial("where", graphMaterial(g));
    }
    h.spawn(sphere, {-5.0f, -5.0f, -5.0f})
        .getComponent<MeshRendererComponent>()
        .materials = {"where"};
    h.spawn(sphere, {5.0f, 5.0f, 5.0f})
        .getComponent<MeshRendererComponent>()
        .materials = {"where"};
    h.frame();
    CHECK(h.renderer.lights.size() == 1u &&
          h.renderer.lights[0].position.x > 4.0f);
  }

  TEST("a pulsing glow should pulse its light, and a glowing rim should light "
       "too")
  {
    Harness h;
    const MeshHandle sphere =
        h.system.meshes().add(meshes::sphere(0.5f, 24, 16, glm::vec3(1.0f)));
    {
      // Emission = Sine Time + 1.
      materialGraph::Graph g;
      const auto out = g.add(std::string(materialGraph::kOutputType), {});
      const auto time = g.add("time", {});
      const auto add = g.add("add", {});
      g.find(add)->inputs[1] = glm::vec4(1.0f);
      g.connect(time, 1, add, 0);
      g.connect(add, 0, out, materialGraph::kEmission);
      h.system.meshes().setMaterial("pulse", graphMaterial(g));
    }
    h.spawn(sphere, {}).getComponent<MeshRendererComponent>().materials = {
        "pulse"};
    h.frame(); // time 0: sin 0 + 1 = 1
    CHECK(h.renderer.lights.size() == 1u &&
          testing::approx(h.renderer.lights[0].intensity, 1.0f, 1e-3f));
    h.scene.update(1.5707963); // a quarter turn: sin + 1 = 2
    h.frame();
    CHECK(h.renderer.lights.size() == 1u &&
          testing::approx(h.renderer.lights[0].intensity, 2.0f, 1e-3f));

    // A Fresnel rim glows only seen edge-on: it gives off light all the same.
    Harness rim;
    const MeshHandle ball =
        rim.system.meshes().add(meshes::sphere(0.5f, 24, 16, glm::vec3(1.0f)));
    {
      materialGraph::Graph g;
      const auto out = g.add(std::string(materialGraph::kOutputType), {});
      const auto fresnel = g.add("fresnel", {});
      g.connect(fresnel, 0, out, materialGraph::kEmission);
      rim.system.meshes().setMaterial("rim", graphMaterial(g));
    }
    rim.spawn(ball, {}).getComponent<MeshRendererComponent>().materials = {
        "rim"};
    rim.frame();
    CHECK(rim.renderer.lights.size() == 1u);
  }

  TEST(
      "the point lights nearest the view should cast shadows, up to the budget")
  {
    Harness h;
    ShadowSettings shadows;
    shadows.pointLights = 2;
    h.system.setShadowSettings(shadows);
    // Five lights along y through the target, one off behind the camera, and
    // one that doesn't cast.
    for (int i = 0; i < 5; ++i)
    {
      Entity lamp = h.scene.createEntity();
      lamp.addComponent<TransformComponent>(
          TransformComponent{{0.0f, static_cast<float>(i) * 3.0f, 0.0f}});
      lamp.addComponent<PointLightComponent>(
          PointLightComponent{glm::vec3(1.0f), 1.0f, 2.0f});
    }
    Entity behind = h.scene.createEntity();
    behind.addComponent<TransformComponent>(
        TransformComponent{{-60.0f, 0.0f, 0.0f}});
    behind.addComponent<PointLightComponent>(
        PointLightComponent{glm::vec3(1.0f), 1.0f, 2.0f});
    Entity plain = h.scene.createEntity();
    plain.addComponent<TransformComponent>(
        TransformComponent{{0.0f, -1.0f, 0.0f}});
    PointLightComponent noShadow{glm::vec3(1.0f), 1.0f, 2.0f};
    noShadow.castsShadow = false;
    plain.addComponent<PointLightComponent>(noShadow);

    // A box by the first light, and one well away from every light.
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    h.spawn(box, {0.0f, 1.0f, 0.0f});
    h.spawn(box, {0.0f, 30.0f, 0.0f});

    h.frame();

    std::vector<float> shadowed;
    for (const PointLight& light : h.renderer.lights)
      if (light.castsShadow)
        shadowed.push_back(light.position.y);
    std::sort(shadowed.begin(), shadowed.end());
    CHECK(shadowed == std::vector<float>({0.0f, 3.0f}));
    CHECK(h.system.stats().shadowedLights == 2u);
    // Only the box near a shadowed light casts into its shadow map.
    std::size_t casters = 0;
    for (const MockRenderer::Batch& batch : h.renderer.batches)
      casters += batch.lightShadowInstances;
    CHECK(casters == 1u);

    shadows.pointLights = 0;
    h.system.setShadowSettings(shadows);
    h.frame();
    CHECK(std::none_of(h.renderer.lights.begin(),
                       h.renderer.lights.end(),
                       [](const PointLight& light)
                       { return light.castsShadow; }));
  }

  TEST("grass's ground should be built once, and again only when its mesh is "
       "turned or scaled; its maps sent up as they change")
  {
    Harness h;
    GrassModule& grassModule = h.system.withModule<GrassModule>(h.renderer);
    const MeshHandle plane = h.system.meshes().add(meshes::plane({4.0f, 4.0f}));
    Entity lawn = h.spawn(plane, {0.0f, 0.0f, 0.0f});
    lawn.addComponent<GrassComponent>(GrassComponent{.density = 20.0f});

    h.frame();
    CHECK(h.renderer.liveGrass.size() == 1u);
    CHECK(h.renderer.grassDrawn.size() == 1u);
    CHECK(grassModule.ground(lawn) &&
          grassModule.ground(lawn)->groundTexels() == 256u);
    CHECK(h.system.stats().grassFields == 1u);
    // Without a material of its own: the module's green. Unpainted: sent up
    // as such.
    CHECK(!h.renderer.grassDrawn.empty() &&
          h.renderer.grassDrawn[0].material.valid() &&
          h.renderer.grassDrawn[0].shape.density == 20.0f);
    CHECK(h.renderer.mapUpdates.size() == 1u &&
          h.renderer.mapUpdates[0].grid.empty());

    // Denser, or moved: the same ground, drawn as it now is.
    const uint32_t first = h.renderer.liveGrass.begin()->first;
    lawn.getComponent<GrassComponent>().density = 40.0f;
    lawn.getComponent<TransformComponent>().position = {10.0f, 0.0f, 0.0f};
    h.frame();
    CHECK(h.renderer.liveGrass.size() == 1u &&
          h.renderer.liveGrass.begin()->first == first);
    CHECK(!h.renderer.grassDrawn.empty() &&
          h.renderer.grassDrawn[0].shape.density == 40.0f &&
          h.renderer.grassDrawn[0].model[3].x == 10.0f);
    CHECK(h.renderer.mapUpdates.size() == 1u); // nothing new

    // Scaled: built again (more texels), the old one freed.
    lawn.getComponent<TransformComponent>().scale = {2.0f, 2.0f, 1.0f};
    h.frame();
    CHECK(h.renderer.liveGrass.size() == 1u &&
          h.renderer.liveGrass.begin()->first != first);
    CHECK(grassModule.ground(lawn)->groundTexels() == 1024u);

    // Painted: its maps go up, all of them.
    GrassMaps bare(grassModule.ground(lawn)->grid());
    bare.fill(GrassMaps::Coverage, 0.0f);
    lawn.getComponent<GrassComponent>().paint = bare.toJson();
    h.frame();
    CHECK(!h.renderer.mapUpdates.empty() &&
          h.renderer.mapUpdates.back().grid == bare.grid() &&
          h.renderer.mapUpdates.back().to == bare.grid().size);
    CHECK(grassModule.sample(lawn, GrassMaps::Coverage, {10.0f, 0.0f, 0.0f}) ==
          0.0f);

    // A game wearing a path: just that part goes up, and isn't saved.
    const std::size_t updates = h.renderer.mapUpdates.size();
    grassModule.stamp(lawn,
                      GrassMaps::Coverage,
                      {11.0f, 1.0f, 0.0f},
                      0.5f,
                      1.0f,
                      GrassMaps::Blend::Toward,
                      1.0f,
                      0.0f);
    CHECK(grassModule.sample(lawn, GrassMaps::Coverage, {11.0f, 1.0f, 0.0f}) ==
          1.0f);
    CHECK(grassModule.sample(lawn, GrassMaps::Coverage, {9.0f, -1.0f, 0.0f}) ==
          0.0f);
    h.frame();
    CHECK(h.renderer.mapUpdates.size() == updates + 1);
    const auto& stamped = h.renderer.mapUpdates.back();
    const glm::ivec2 area = stamped.to - stamped.from;
    CHECK(area.x > 0 && area.x <= 6 && area.y > 0 && area.y <= 6);
    CHECK(lawn.getComponent<GrassComponent>().paint == bare.toJson());

    // Painting over it starts the maps over from the paint: unpainted.
    lawn.getComponent<GrassComponent>().paint = nlohmann::json::object();
    h.frame();
    CHECK(h.renderer.mapUpdates.back().grid.empty());
    CHECK(grassModule.sample(lawn, GrassMaps::Coverage, {10.0f, 0.0f, 0.0f}) ==
          1.0f);
    // A script asking for unpainted grass's maps gets them, full.
    GrassMaps* maps = grassModule.maps(lawn);
    CHECK(maps && maps->grid() == grassModule.ground(lawn)->grid());

    // Gone with its component.
    lawn.removeComponent<GrassComponent>();
    h.frame();
    CHECK(h.renderer.liveGrass.empty());
    CHECK(h.renderer.grassDrawn.empty());
    CHECK(grassModule.maps(lawn) == nullptr);
  }

  TEST("grass on a terrain should grow only where its layers let it, as the "
       "material drawn says")
  {
    Harness h;
    GrassModule& grassModule = h.system.withModule<GrassModule>(h.renderer);
    const MeshHandle plane =
        h.system.meshes().add(meshes::plane({10.0f, 10.0f}));
    Entity lawn = h.spawn(plane, {0.0f, 0.0f, 0.0f});
    lawn.addComponent<GrassComponent>(GrassComponent{.density = 20.0f});
    // Dirt (no grass) painted over the left half (u below 0.5: x below 0).
    auto terrain = std::make_shared<TerrainMaterial>();
    auto splat = blankSplat(8);
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 4; ++x)
        splat->rgba[(std::size_t(y) * 8 + x) * 4] = 255;
    terrain->splat = splat;
    terrain->layers = {TerrainLayer{}, TerrainLayer{.grass = 0.0f}};
    MeshMaterial ground;
    ground.terrain = terrain;
    h.system.meshes().setMaterial("ground", ground);
    lawn.getComponent<MeshRendererComponent>().materials = {"ground"};

    h.frame();
    CHECK(!h.renderer.mapUpdates.empty());
    const auto coverageAt = [&](float x)
    {
      const auto& update = h.renderer.mapUpdates.back();
      const glm::ivec2 texel(
          glm::floor(update.grid.toTexels(glm::vec2(x, 0.0f))));
      const int width = update.to.x - update.from.x;
      return update.shape[(std::size_t(texel.y - update.from.y) * width +
                           (texel.x - update.from.x)) *
                          4];
    };
    CHECK(!h.renderer.mapUpdates.back().grid.empty());
    CHECK(coverageAt(-3.0f) < 10);
    CHECK(coverageAt(3.0f) == 255);
    // The maps themselves keep it all (the game's to read and wear).
    CHECK(grassModule.maps(lawn) &&
          testing::approx(grassModule.sample(
                              lawn, GrassMaps::Coverage, {-3.0f, 0.0f, 0.0f}),
                          1.0f));

    // Painted: grown again once told.
    paintSplat(*splat, {0.75f, 0.5f}, {0.3f, 0.6f}, 1, 1.0f, 0.0f);
    grassModule.terrainPainted(lawn);
    h.frame();
    CHECK(coverageAt(3.0f) < 10);

    // A plain material again: full grass, as unpainted grass is.
    h.system.meshes().setMaterial("ground", MeshMaterial{});
    h.renderer.mapUpdates.clear();
    h.frame();
    CHECK(!h.renderer.mapUpdates.empty() &&
          h.renderer.mapUpdates.back().grid.empty());
  }

  TEST("rigid bodies and grass benders should flatten grass, unless they opt "
       "out")
  {
    Harness h;
    GrassModule& grassModule = h.system.withModule<GrassModule>(h.renderer);
    const MeshHandle plane =
        h.system.meshes().add(meshes::plane({10.0f, 10.0f}));
    Entity lawn = h.spawn(plane, {0.0f, 0.0f, 0.0f});
    lawn.addComponent<GrassComponent>(GrassComponent{.density = 30.0f});

    // A ball resting at (-3, 0), a crate (box) at (3, 0), a ball that opts
    // out at (0, 3), and a bender with no physics at (0, -3).
    const MeshHandle ballMesh =
        h.system.meshes().add(meshes::sphere(0.5f, 12, 8));
    Entity ball = h.spawn(ballMesh, {-3.0f, 0.0f, 0.0f});
    ball.addComponent<RigidBody3DComponent>();
    ball.addComponent<SphereCollider3DComponent>(
        SphereCollider3DComponent{0.5f, {0.0f, 0.0f, 0.5f}});
    Entity crate = h.spawn(ballMesh, {3.0f, 0.0f, 0.0f});
    crate.addComponent<RigidBody3DComponent>();
    crate.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.5f}});
    Entity aloof = h.spawn(ballMesh, {0.0f, 3.0f, 0.0f});
    RigidBody3DComponent noBending;
    noBending.bendsGrass = false;
    aloof.addComponent<RigidBody3DComponent>(noBending);
    aloof.addComponent<SphereCollider3DComponent>(
        SphereCollider3DComponent{0.5f, {0.0f, 0.0f, 0.5f}});
    Entity walker = h.scene.createEntity();
    walker.addComponent<TransformComponent>(
        TransformComponent{{0.0f, -3.0f, 0.4f}});
    walker.addComponent<GrassBenderComponent>(
        GrassBenderComponent{.radius = 0.4f});

    h.frame(); // builds the field
    h.frame(); // and presses it
    CHECK(grassModule.benders().size() == 3u);
    CHECK(grassModule.flattened(lawn, {-3.0f, 0.0f}) > 0.9f);
    CHECK(grassModule.flattened(lawn, {3.0f, 0.0f}) > 0.9f);
    CHECK(grassModule.flattened(lawn, {0.0f, -3.0f}) > 0.9f);
    CHECK(grassModule.flattened(lawn, {0.0f, 3.0f}) == 0.0f);
    CHECK(grassModule.flattened(lawn, {0.0f, 0.0f}) == 0.0f);
    CHECK(!h.renderer.trampleUpdates.empty());

    // The ball rolls away: where it was springs back over its recovery time.
    ball.getComponent<TransformComponent>().position = {-3.0f, -3.0f, 5.0f};
    h.scene.update(1.0);
    h.scene.render();
    const float rising = grassModule.flattened(lawn, {-3.0f, 0.0f});
    CHECK(rising > 0.5f && rising < 0.8f); // a third of the way up
    h.scene.update(3.0);
    h.scene.render();
    CHECK(grassModule.flattened(lawn, {-3.0f, 0.0f}) == 0.0f);
  }

  TEST("a grass bender on a mesh should fit it, wherever and however big it "
       "is; else take its radius")
  {
    Harness h;
    GrassModule& grassModule = h.system.withModule<GrassModule>(h.renderer);
    // A wheel: a cylinder 0.4 across, its middle 0.2 up, scaled 1.5.
    const MeshHandle wheelMesh =
        h.system.meshes().add(meshes::cylinder(0.2f, 0.3f, 12));
    Entity wheel = h.spawn(wheelMesh, {2.0f, 1.0f, 0.0f});
    wheel.getComponent<TransformComponent>().scale = glm::vec3(1.5f);
    wheel.addComponent<GrassBenderComponent>();
    Entity unfitted = h.spawn(wheelMesh, {-2.0f, 0.0f, 0.0f});
    unfitted.addComponent<GrassBenderComponent>(
        GrassBenderComponent{.fitMesh = false, .radius = 0.7f});

    h.frame(); // the module learns the meshes
    h.frame();
    CHECK(grassModule.benders().size() == 2u);
    for (const GrassBender& bender : grassModule.benders())
      if (bender.center.x > 0.0f)
      {
        // Its widest: 0.4 x 1.5 across, so 0.3; its middle where it's placed.
        CHECK(testing::approx(bender.radius, 0.3f));
        CHECK(testing::approx(bender.center.x, 2.0f));
        CHECK(testing::approx(bender.center.y, 1.0f));
      }
      else
        CHECK(testing::approx(bender.radius, 0.7f));
  }

  TEST("UI layers should be drawn over the scene, their buttons clicked, and "
       "clicks on them kept from the game")
  {
    // A layer with a button in the top left, and an object that sees what
    // input the game gets.
    struct Hud : public ui::Layer
    {
      int clicks = 0;
      ui::View body() override
      {
        using namespace ui;
        return VStack{Text("Day 1"), Button("Go", [this] { ++clicks; })}
            .padding()
            .background(Panel())
            .align(Alignment::TopLeading);
      }
    };
    struct Game : public GameObject
    {
      int presses = 0;
      void onProcessInput(const Input& input) override
      {
        presses += input.mouse().mousePressed(MouseButton::Left);
      }
    };

    Harness h;
    UiModule& ui = h.system.withModule<UiModule>(h.scene);
    Hud& hud = h.scene.createObject<Hud>();
    Game& game = h.scene.createObject<Game>();
    h.frame();
    CHECK(!h.renderer.uiDrawn.empty());
    const ui::Hit* button = nullptr;
    for (const ui::Hit& hit : ui.hits())
      if (hit.action)
        button = &hit;
    CHECK(button != nullptr);
    if (!button)
      return testing::report("renderSystemTests");
    const glm::vec2 on(button->rect.x + button->rect.width * 0.5f,
                       button->rect.y + button->rect.height * 0.5f);

    // The scene's input, the UI first (as StandardScene does).
    Input input;
    input.mouse().setViewSize(h.renderer.viewport.x, h.renderer.viewport.y);
    const auto send = [&]
    {
      Input seen = input;
      ui.handleInput(seen);
      for (GameObject* object : std::vector<GameObject*>{&game})
        static_cast<Game*>(object)->onProcessInput(seen);
      input.mouse().beginFrame();
    };

    // Pressed and let go on the button: clicked, and the game saw nothing.
    input.mouse().setPosition(static_cast<int>(on.x), static_cast<int>(on.y));
    input.mouse().pressButton(MouseButton::Left);
    send();
    input.mouse().releaseButton(MouseButton::Left);
    send();
    CHECK(hud.clicks == 1);
    CHECK(game.presses == 0);

    // Off the UI: the game's click.
    input.mouse().setPosition(static_cast<int>(h.renderer.viewport.x - 5),
                              static_cast<int>(h.renderer.viewport.y - 5));
    input.mouse().pressButton(MouseButton::Left);
    send();
    input.mouse().releaseButton(MouseButton::Left);
    send();
    CHECK(hud.clicks == 1);
    CHECK(game.presses == 1);

    // Hidden: nothing drawn.
    hud.visible = false;
    h.frame();
    CHECK(h.renderer.uiDrawn.empty());
  }

  TEST("a mesh's icon should be drawn once from the scene's meshes, and none "
       "be made for a mesh there isn't")
  {
    Harness h;
    UiModule& ui = h.system.withModule<UiModule>(h.scene);
    const MeshHandle crate = h.system.meshes().add(
        meshes::box({1.0f, 1.0f, 1.0f}, glm::vec3(0.8f, 0.3f, 0.2f)),
        "test:crate");
    h.frame(); // the module learns the meshes
    const UiTexture* icon = ui.image("test:crate", true);
    CHECK(icon != nullptr);
    if (icon)
    {
      CHECK(icon->size == glm::ivec2(128, 128));
      // Drawn, on nothing: some pixels solid, the corners clear.
      bool solid = false;
      for (std::size_t i = 3; i < icon->pixels.size(); i += 4)
        solid = solid || icon->pixels[i] > 200;
      CHECK(solid && icon->pixels[3] == 0);
      CHECK(ui.image("test:crate", true) == icon); // once
      // By handle (Image::mesh(MeshHandle)), the same picture.
      CHECK(ui.image("#" + std::to_string(crate.id), true) != nullptr);
    }
    CHECK(ui.image("test:nothing", true) == nullptr);
    CHECK(ui.image("no/such/file.png", false) == nullptr);
  }

  TEST("the clock should run while animating, and hold while not")
  {
    Harness h;
    h.scene.update(0.5);
    h.scene.render();
    CHECK(testing::approx(h.renderer.frame.time, 0.5f));
    h.system.setAnimating(false);
    h.scene.update(2.0);
    h.scene.render();
    CHECK(testing::approx(h.renderer.frame.time, 0.5f)); // held
    h.system.setAnimating(true);
    h.scene.update(0.25);
    h.scene.render();
    CHECK(testing::approx(h.renderer.frame.time, 0.75f)); // on from there
  }

  TEST("the scene's wind should reach the renderer")
  {
    Harness h;
    h.system.setWindSettings(
        {.direction = 90.0f, .strength = 0.7f, .gustiness = 0.2f});
    h.frame();
    CHECK(testing::approx(h.renderer.frame.wind.direction, 90.0f));
    CHECK(testing::approx(h.renderer.frame.wind.strength, 0.7f));
  }

  TEST("the scene's haze, and its camera's depth of field focused where the "
       "view is, should reach the renderer")
  {
    Harness h;
    h.system.setHazeSettings({.strength = 0.6f, .distance = 80.0f});
    h.frame();
    CHECK(testing::approx(h.renderer.frame.haze.strength, 0.6f));
    CHECK(testing::approx(h.renderer.frame.haze.distance, 80.0f));
    CHECK(!h.renderer.frame.depthOfField.enabled); // no camera: none
    CHECK(testing::approx(
        h.renderer.frame.focusDistance, h.system.camera().distance));

    Entity eye = h.scene.createEntity();
    eye.addComponent<TransformComponent>();
    CameraComponent lens;
    lens.depthOfField = true;
    lens.focusDistance = 7.0f;
    lens.focusRange = 3.0f;
    lens.focusBlur = 2.0f;
    eye.addComponent<CameraComponent>(lens);
    h.frame();
    CHECK(h.renderer.frame.depthOfField.enabled);
    CHECK(testing::approx(h.renderer.frame.depthOfField.range, 3.0f));
    CHECK(testing::approx(h.renderer.frame.depthOfField.blur, 2.0f));
    CHECK(testing::approx(h.renderer.frame.focusDistance, 7.0f));

    // Looking around freely (the editor), focused on its own target.
    h.system.setFollowSceneCamera(false);
    h.system.camera().distance = 12.0f;
    h.frame();
    CHECK(h.renderer.frame.depthOfField.enabled);
    CHECK(testing::approx(h.renderer.frame.focusDistance, 12.0f));
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
    MockRenderer renderer;
    {
      AssetStore store;
      Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
      auto& system = scene.addSystem<RenderSystem>(renderer);
      system.meshes().add(meshes::box({1, 1, 1}));
      system.meshes().add(meshes::plane({1, 1}));
      CHECK(renderer.live.size() == 2u);
    }
    CHECK(renderer.live.empty());
  }

  TEST("a mesh loading in the background should draw once it has loaded, under "
       "the same handle")
  {
    Harness h;
    MeshLibrary& meshes = h.system.meshes();
    const MeshData box = meshes::box({1, 1, 1});
    bool ready = false;
    MeshLibrary::Provider provider;
    provider.load = [](MeshLibrary& library, std::string_view name)
    { return library.pending(name); };
    provider.loaded = [&](std::string_view, bool& waiting) -> const MeshData*
    {
      waiting = !ready;
      return ready ? &box : nullptr;
    };
    meshes.setProvider(provider);

    const MeshHandle handle = meshes.resolve("asset:crate");
    CHECK(handle.valid());
    CHECK(meshes.isPending(handle));
    CHECK(meshes.resolve("asset:crate") == handle); // one stand-in per name
    h.spawn(handle, {0.0f, 0.0f, 0.0f});
    h.frame();
    CHECK(h.renderer.batches.empty()); // nothing to draw yet
    CHECK(h.renderer.live.empty());

    meshes.fulfilPending(); // still loading: unchanged
    CHECK(meshes.isPending(handle));

    ready = true;
    meshes.fulfilPending();
    CHECK(!meshes.isPending(handle));
    CHECK(!meshes.isMissing(handle));
    CHECK(meshes.pendingCount() == 0u);
    CHECK(meshes.bounds(handle) != nullptr);
    CHECK(meshes.nameOf(handle) ==
          "asset:crate"); // saved scenes keep naming it
    h.frame();
    CHECK(h.renderer.batches.size() == 1u);
    CHECK(h.renderer.batches[0].mesh ==
          meshes.gpuHandle(handle)); // the renderer's own handle
    CHECK(h.renderer.live.size() == 1u);

    meshes.remove(handle);
    CHECK(h.renderer.live.empty());
  }

  TEST("a mesh that fails to load in the background should stay a stand-in")
  {
    Harness h;
    MeshLibrary& meshes = h.system.meshes();
    MeshLibrary::Provider provider;
    provider.load = [](MeshLibrary& library, std::string_view name)
    { return library.pending(name); };
    provider.loaded = [](std::string_view, bool& waiting) -> const MeshData*
    {
      waiting = false;
      return nullptr;
    };
    meshes.setProvider(provider);
    const MeshHandle handle = meshes.resolve("asset:broken");
    meshes.fulfilPending();
    CHECK(!meshes.isPending(handle));
    CHECK(meshes.isMissing(handle));
    CHECK(meshes.bounds(handle) == nullptr);
  }

  TEST("kept vertices should give a mesh's data back")
  {
    Harness h;
    MeshLibrary::setKeepVertices(true);
    const MeshData box = meshes::box({1, 2, 3}, glm::vec3(0.5f));
    const MeshHandle kept = h.system.meshes().add(box);
    MeshLibrary::setKeepVertices(false);
    const MeshHandle notKept = h.system.meshes().add(box);
    const std::optional<MeshData> data = h.system.meshes().meshData(kept);
    CHECK(data && data->vertices.size() == box.vertices.size() &&
          data->indices == box.indices);
    CHECK(data && data->vertices[0].color == box.vertices[0].color);
    CHECK(!h.system.meshes().meshData(notKept));
  }

  TEST("an empty mesh should not upload")
  {
    Harness h;
    CHECK(!h.system.meshes().add(MeshData{}).valid());
    CHECK(h.system.meshes().size() == 0u);
  }

  TEST("objects with their own materials should batch apart, uploading each "
       "once")
  {
    Harness h;
    MeshLibrary::Provider provider;
    int loads = 0;
    provider.loadMaterial =
        [&loads](std::string_view name) -> std::shared_ptr<const MeshMaterial>
    {
      ++loads;
      return name == "wood" ? std::make_shared<MeshMaterial>() : nullptr;
    };
    h.system.meshes().setProvider(provider);

    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    h.spawn(box, {0.0f, -2.0f, 0.0f});
    for (const float y : {0.0f, 2.0f})
      h.spawn(box, {0.0f, y, 0.0f})
          .getComponent<MeshRendererComponent>()
          .materials = {"wood"};
    // A missing material, or an empty slot, is the mesh's own: batches with the
    // first.
    h.spawn(box, {0.0f, 4.0f, 0.0f})
        .getComponent<MeshRendererComponent>()
        .materials = {"missing"};
    h.spawn(box, {0.0f, 6.0f, 0.0f})
        .getComponent<MeshRendererComponent>()
        .materials = {""};

    h.frame();
    h.frame();

    CHECK(h.renderer.batches.size() == 2u);
    const auto wood = std::find_if(h.renderer.batches.begin(),
                                   h.renderer.batches.end(),
                                   [](const auto& batch)
                                   { return !batch.materials.empty(); });
    CHECK(wood != h.renderer.batches.end());
    if (wood != h.renderer.batches.end())
    {
      CHECK(wood->instances == 2u);
      CHECK(wood->materials.size() == 1u);
    }
    CHECK(h.renderer.liveMaterials.size() == 1u);
    CHECK(loads == 2); // wood and missing, each asked for once
  }

  TEST("a material set from game code should be made once, then updated in "
       "place")
  {
    Harness h;
    MeshMaterial painted;
    painted.baseColor = glm::vec4(0.5f);
    const MaterialHandle first =
        h.system.meshes().setMaterial("game:painted", painted);
    CHECK(first.valid());
    CHECK(h.system.meshes().material("game:painted") == first);
    painted.baseColor = glm::vec4(1.0f);
    const int updates = h.renderer.materialUpdates;
    CHECK(h.system.meshes().setMaterial("game:painted", painted) == first);
    CHECK(h.renderer.materialUpdates == updates + 1);
    CHECK(h.renderer.liveMaterials.size() == 1u);
  }

  TEST("reloading a material should update it in place")
  {
    Harness h;
    MeshLibrary::Provider provider;
    provider.loadMaterial = [](std::string_view)
    { return std::make_shared<const MeshMaterial>(); };
    h.system.meshes().setProvider(provider);
    const MaterialHandle wood = h.system.meshes().material("wood");
    h.system.meshes().reloadMaterial("wood");
    CHECK(h.system.meshes().material("wood") == wood);
    CHECK(h.renderer.materialUpdates == 1);
    h.system.meshes().reloadMaterial("never loaded"); // ignored
    CHECK(h.renderer.materialUpdates == 1);
  }

  TEST("a visual style should stand in for the scene's look while it's on, "
       "and re-balance its light")
  {
    Harness h;
    std::vector<std::string> asked;
    VisualStyle style;
    style.grading.toneMapper = ToneMapper::Aces;
    style.grading.exposure = 0.5f;
    style.sunIntensity = 2.0f;
    style.ambientTint = {0.5f, 1.0f, 1.0f};
    style.skyIntensity = 0.5f;
    style.bloom = BloomSettings{.intensity = 0.1f, .threshold = 2.0f};
    style.shadowSoftness = 3.0f;
    h.system.setStyleSource(
        [&](std::string_view name) -> std::optional<VisualStyle>
        {
          asked.emplace_back(name);
          if (name == "asset:real")
            return style;
          return std::nullopt;
        });

    SceneLighting lighting;
    lighting.skyColor = {0.4f, 0.6f, 0.8f};
    h.system.setLighting(lighting);
    h.system.setHazeSettings({.strength = 0.3f, .distance = 90.0f});
    ColorGrading own;
    own.saturation = 1.5f;
    h.system.setColorGrading(own);

    // No style: the scene's own look, its light as it is.
    h.frame();
    CHECK(h.renderer.frame.grading == own);
    CHECK(h.renderer.frame.lightBalance.neutral());
    CHECK(testing::approx(h.renderer.frame.lighting.skyColor.g, 0.6f));
    CHECK(testing::approx(h.renderer.frame.bloom.intensity, 0.6f));

    // On: its grading, bloom and shadow softness; the haze it leaves alone
    // stays the scene's; the sun doubled, ambient tinted, the sky halved.
    CHECK(h.system.setVisualStyle("asset:real"));
    CHECK(h.system.visualStyle() != nullptr);
    h.frame();
    CHECK(h.renderer.frame.grading == style.grading);
    CHECK(testing::approx(h.renderer.frame.bloom.threshold, 2.0f));
    CHECK(testing::approx(h.renderer.frame.shadows.softness, 3.0f));
    CHECK(testing::approx(h.renderer.frame.haze.strength, 0.3f));
    CHECK(h.renderer.frame.lightBalance.sun == glm::vec3(2.0f));
    CHECK(h.renderer.frame.lightBalance.ambient == style.ambientTint);
    CHECK(testing::approx(h.renderer.frame.lighting.skyColor.g, 0.3f));
    // The scene's own settings are untouched: they come back with it off.
    CHECK(h.system.colorGrading() == own);
    CHECK(testing::approx(h.system.lighting().skyColor.g, 0.6f));

    // A day/night cycle setting the lighting each frame stays re-balanced.
    lighting.skyColor = {1.0f, 1.0f, 1.0f};
    h.system.setLighting(lighting);
    h.frame();
    CHECK(testing::approx(h.renderer.frame.lighting.skyColor.r, 0.5f));

    // One it can't load: the name kept, the scene's own look.
    CHECK(!h.system.setVisualStyle("asset:gone"));
    CHECK(h.system.visualStyleName() == "asset:gone");
    h.frame();
    CHECK(h.renderer.frame.grading == own);

    // Off.
    CHECK(h.system.setVisualStyle(""));
    h.frame();
    CHECK(h.renderer.frame.grading == own);
    CHECK(h.renderer.frame.lightBalance.neutral());
    CHECK(testing::approx(
        h.renderer.frame.shadows.softness, ShadowSettings{}.softness));

    // Given as is (an edit being previewed), not loaded.
    const std::size_t asks = asked.size();
    VisualStyle edited = style;
    edited.grading.exposure = -1.0f;
    h.system.setVisualStyle("asset:real", edited);
    h.frame();
    CHECK(asked.size() == asks);
    CHECK(testing::approx(h.renderer.frame.grading.exposure, -1.0f));
    // Reloading reads it again (the saved one).
    h.system.reloadVisualStyle();
    h.frame();
    CHECK(testing::approx(h.renderer.frame.grading.exposure, 0.5f));
  }

  TEST("a physical sky should light the frame from the sun's direction, "
       "working its diffuse light out again only when the sun has moved")
  {
    Harness h;
    SceneLighting lighting;
    lighting.sun.direction = {0.5f, -0.2f, 0.8f};
    h.system.setLighting(lighting);

    // Flat (the default): no model.
    h.frame();
    CHECK(!h.renderer.frame.sky.physical);

    SkySettings physical;
    physical.model = SkyModel::Physical;
    h.system.setSkySettings(physical);
    h.frame();
    const FrameSky first = h.renderer.frame.sky;
    CHECK(first.physical != nullptr);
    CHECK(first.sunLight.r > 0.5f);
    CHECK(first.irradiance.at({0, 0, 1}).b > 0.05f);
    CHECK(glm::length(first.toSun - glm::normalize(lighting.sun.direction)) <
          1e-5f);

    // The same sun: nothing worked out again; the same model kept.
    h.frame();
    CHECK(h.renderer.frame.sky.revision == first.revision);
    CHECK(h.renderer.frame.sky.physical == first.physical);
    // A hair's move: still not; a degree: again.
    lighting.sun.direction = {0.5f, -0.2001f, 0.8f};
    h.system.setLighting(lighting);
    h.frame();
    CHECK(h.renderer.frame.sky.revision == first.revision);
    // (In the background: the frame keeps the old until it's ready.)
    lighting.sun.direction = {0.5f, -0.2f, 0.75f};
    h.system.setLighting(lighting);
    h.frame();
    h.system.finishSkyWork();
    h.frame();
    CHECK(h.renderer.frame.sky.revision != first.revision);
    CHECK(glm::length(h.renderer.frame.sky.toSun -
                      glm::normalize(lighting.sun.direction)) < 1e-5f);

    // Set: no sun, no shadows.
    lighting.sun.direction = {0.5f, -0.2f, -0.3f};
    h.system.setLighting(lighting);
    h.frame();
    CHECK(h.renderer.frame.sky.sunLight == glm::vec3(0.0f));
    CHECK(!h.renderer.frame.shadows.enabled);

    // A full moon up: it lights the frame in the sun's place, casting
    // shadows, and the night sky glows brighter under it.
    lighting.moon.direction = {-0.3f, -0.2f, 0.8f};
    lighting.moon.phase = 1.0f;
    h.system.setLighting(lighting);
    // (Twice: the setting sun's may still have been being worked out.)
    for (int i = 0; i < 2; ++i)
    {
      h.frame();
      h.system.finishSkyWork();
    }
    h.frame();
    const FrameSky moonlit = h.renderer.frame.sky;
    CHECK(moonlit.moonLit);
    CHECK(moonlit.sunLight.b > 0.002f);
    CHECK(moonlit.night > 1.05f);
    CHECK(h.renderer.frame.shadows.enabled);
    CHECK(glm::length(h.renderer.frame.lighting.sun.direction -
                      glm::normalize(lighting.moon.direction)) < 1e-5f);
    CHECK(glm::length(moonlit.toSun -
                      glm::normalize(glm::vec3(0.5f, -0.2f, -0.3f))) < 1e-5f);
    // A half moon gives less; clouds less again.
    lighting.moon.phase = 0.5f;
    h.system.setLighting(lighting);
    h.frame();
    CHECK(h.renderer.frame.sky.sunLight.b < moonlit.sunLight.b * 0.5f);
    lighting.moon.phase = 1.0f;
    lighting.cloudCover = 1.0f;
    h.system.setLighting(lighting);
    h.frame();
    CHECK(h.renderer.frame.sky.sunLight.b < moonlit.sunLight.b * 0.5f);
    CHECK(h.renderer.frame.sky.cloudCover == 1.0f);
    // Set too: nothing shines.
    lighting.cloudCover = 0.0f;
    lighting.moon.direction = {-0.3f, -0.2f, -0.5f};
    h.system.setLighting(lighting);
    h.frame();
    CHECK(!h.renderer.frame.sky.moonLit);
    CHECK(!h.renderer.frame.shadows.enabled);

    // A style's sky replaces the scene's; its sky balance scales it.
    VisualStyle style;
    style.sky = SkySettings{};
    h.system.setVisualStyle("asset:flat", style);
    h.frame();
    CHECK(!h.renderer.frame.sky.physical);
    style.sky = physical;
    style.skyIntensity = 0.5f;
    h.system.setVisualStyle("asset:physical", style);
    h.system.setSkySettings({});
    h.frame();
    CHECK(h.renderer.frame.sky.physical != nullptr);
    CHECK(h.renderer.frame.sky.scale == glm::vec3(0.5f));

    // A 2D game's sky is always flat.
    h.system.setSpace2D(Space2D{});
    h.frame();
    CHECK(!h.renderer.frame.sky.physical);
  }

  TEST("shadow cascades should reach along the view when asked for, and the "
       "one map stay as ever otherwise")
  {
    Harness h;
    SceneLighting lighting;
    lighting.sun.direction = {0.4f, -0.3f, 0.8f};
    h.system.setLighting(lighting);

    h.frame();
    CHECK(h.renderer.frame.shadows.enabled);
    CHECK(h.renderer.frame.cascades.count == 0);
    const glm::mat4 single = h.renderer.frame.lightViewProjection;

    VisualStyle style;
    style.shadowCascades = 4;
    style.shadowCascadeDistance = 150.0f;
    h.system.setVisualStyle("asset:far", style);
    h.frame();
    const SunCascades& cascades = h.renderer.frame.cascades;
    CHECK(cascades.count == 4);
    CHECK(cascades.cascades[3].splitFar <= 150.0f + 1e-3f);
    CHECK(h.renderer.frame.shadows.cascades == 4u);

    // Off again: the one map, exactly as before.
    h.system.setVisualStyle("");
    h.frame();
    CHECK(h.renderer.frame.cascades.count == 0);
    CHECK(h.renderer.frame.lightViewProjection == single);

    // A 2D game: the one map.
    ShadowSettings shadows = h.system.shadowSettings();
    shadows.cascades = 3;
    h.system.setShadowSettings(shadows);
    h.frame();
    CHECK(h.renderer.frame.cascades.count == 3);
    h.system.setSpace2D(Space2D{});
    h.frame();
    CHECK(h.renderer.frame.cascades.count == 0);
  }

  TEST("materials should draw as their variants for the visual style's tag, "
       "switching live, without loading anything more per frame")
  {
    Harness h;
    MeshLibrary& library = h.system.meshes();
    MeshLibrary::Provider provider;
    int loads = 0;
    provider.loadMaterial =
        [&loads](std::string_view name) -> std::shared_ptr<const MeshMaterial>
    {
      ++loads;
      auto material = std::make_shared<MeshMaterial>();
      if (name == "turf")
        material->variants = {
            {"realistic", "turf real"}, {"painted", "turf painted"}};
      if (name == "turf real" || name == "turf painted")
        material->roughness = 0.5f;
      if (name == "turf real") // not followed (a variant's own variant)
        material->variants = {{"realistic", "turf painted"}};
      return name == "nothing" ? nullptr : material;
    };
    library.setProvider(provider);

    const MeshHandle box = library.add(meshes::box({1, 1, 1}));
    h.spawn(box, {0.0f, 0.0f, 0.0f})
        .getComponent<MeshRendererComponent>()
        .materials = {"turf"};
    h.spawn(box, {0.0f, 2.0f, 0.0f})
        .getComponent<MeshRendererComponent>()
        .materials = {"stone"};
    GrassModule& grassModule = h.system.withModule<GrassModule>(h.renderer);
    (void)grassModule;
    const MeshHandle plane = library.add(meshes::plane({4.0f, 4.0f}));
    Entity lawn = h.spawn(plane, {0.0f, 0.0f, 0.0f});
    lawn.addComponent<GrassComponent>(
        GrassComponent{.material = "turf", .density = 20.0f});

    const auto drawnMaterials = [&]
    {
      std::vector<MaterialHandle> out;
      for (const auto& batch : h.renderer.batches)
        out.insert(out.end(), batch.materials.begin(), batch.materials.end());
      return out;
    };
    const auto has =
        [](const std::vector<MaterialHandle>& list, MaterialHandle material)
    { return std::find(list.begin(), list.end(), material) != list.end(); };

    h.frame();
    const MaterialHandle turf = library.material("turf");
    const MaterialHandle stone = library.material("stone");
    CHECK(has(drawnMaterials(), turf) && has(drawnMaterials(), stone));
    CHECK(h.renderer.grassDrawn.size() == 1u &&
          h.renderer.grassDrawn[0].material == turf);
    CHECK(loads == 2); // no variant loaded while none is asked for

    // The style's tag: turf draws as its variant (grass too), stone as itself.
    VisualStyle realistic;
    realistic.materialVariant = "realistic";
    h.system.setVisualStyle("asset:realistic", realistic);
    CHECK(library.materialVariant() == "realistic");
    h.frame();
    const MaterialHandle real = library.material("turf");
    CHECK(real.valid() && real != turf);
    CHECK(library.materialData(real)->roughness == 0.5f);
    CHECK(has(drawnMaterials(), real) && !has(drawnMaterials(), turf));
    CHECK(has(drawnMaterials(), stone));
    CHECK(h.renderer.grassDrawn.size() == 1u &&
          h.renderer.grassDrawn[0].material == real);
    // The variant asked as itself doesn't follow its own variant.
    CHECK(library.material("turf real") == real);
    const int loaded = loads;
    for (int i = 0; i < 3; ++i)
      h.frame();
    CHECK(loads == loaded); // nothing more loads frame to frame

    // Another tag, then none: back to the material itself, live.
    VisualStyle painted;
    painted.materialVariant = "painted";
    h.system.setVisualStyle("asset:painted", painted);
    h.frame();
    const MaterialHandle paint = library.material("turf");
    CHECK(paint != real && paint != turf);
    CHECK(has(drawnMaterials(), paint));
    h.system.setVisualStyle("");
    h.frame();
    CHECK(library.material("turf") == turf);
    CHECK(has(drawnMaterials(), turf) && !has(drawnMaterials(), paint));

    // A tag none has, and a variant that can't load: themselves.
    library.setMaterialVariant("unknown");
    CHECK(library.material("turf") == turf);
    MeshMaterial broken;
    broken.variants = {{"realistic", "nothing"}};
    const MaterialHandle game = library.setMaterial("game:broken", broken);
    library.setMaterialVariant("realistic");
    CHECK(library.material("game:broken") == game);

    // Materials loaded after the tag is set resolve straight away.
    CHECK(library.material("turf") == real);

    // Every material (variants included) is freed with the library.
    const std::size_t live = h.renderer.liveMaterials.size();
    CHECK(live >= 5u);
  }

  TEST("a style's material overrides should draw a game's own material as "
       "another, and a terrain standing in for a painted one keep its paint")
  {
    Harness h;
    MeshLibrary& library = h.system.meshes();
    const auto image = [](uint8_t value)
    {
      auto texture = std::make_shared<TextureData>();
      texture->width = texture->height = 1;
      texture->rgba = {value, value, value, 255};
      return texture;
    };
    // A game paints its ground in code; a terrain to stand in for it.
    MeshMaterial painted;
    painted.baseTexture = image(200);
    const MaterialHandle ground = library.setMaterial("game:ground", painted);
    MeshMaterial plain;
    const MaterialHandle other = library.setMaterial("game:plain", plain);
    MeshMaterial terrain;
    auto layers = std::make_shared<TerrainMaterial>();
    layers->layers.push_back({.name = "Grass"});
    terrain.terrain = layers;
    const MaterialHandle field = library.setMaterial("terrain", terrain);
    CHECK(library.material("game:ground") == ground);
    const std::size_t live = h.renderer.liveMaterials.size();

    VisualStyle style;
    style.materials = {{"game:ground", "terrain"}, {"game:plain", "missing"}};
    h.system.setVisualStyle("asset:style", style);
    const MaterialHandle drawn = library.material("game:ground");
    CHECK(drawn.valid() && drawn != ground && drawn != field);
    const MeshMaterial* standIn = library.materialData(drawn);
    CHECK(standIn && standIn->terrain &&
          standIn->terrain->tint == painted.baseTexture);
    CHECK(standIn && standIn->terrain->layers.size() == 1u);
    // An override that can't load: itself.
    CHECK(library.material("game:plain") == other);

    // Painted again: the same stand-in, its tint the new paint.
    painted.baseTexture = image(90);
    library.setMaterial("game:ground", painted);
    CHECK(library.material("game:ground") == drawn);
    CHECK(library.materialData(drawn)->terrain->tint == painted.baseTexture);

    // No style: itself again, the stand-in let go.
    h.system.setVisualStyle("");
    CHECK(library.material("game:ground") == ground);
    CHECK(h.renderer.liveMaterials.size() == live);
  }

  TEST("a mesh's sections should be its material slots")
  {
    Harness h;
    MeshData model = meshes::box({1, 1, 1});
    MeshMaterial red;
    red.baseColor = {1, 0, 0, 1};
    model.sections = {{0, 18, MeshMaterial{}, "Wood"}, {18, 18, red, "Roof"}};
    const MeshHandle barn = h.system.meshes().add(model);
    const std::vector<MeshSection> slots = h.system.meshes().sections(barn);
    CHECK(slots.size() == 2u);
    if (slots.size() == 2u)
    {
      CHECK(slots[0].name == "Wood");
      CHECK(slots[1].material.baseColor.r == 1.0f);
    }
    // No sections: one slot, the default material.
    CHECK(h.system.meshes()
              .sections(h.system.meshes().add(meshes::box({1, 1, 1})))
              .size() == 1u);
    CHECK(h.system.meshes().sections(MeshHandle{999}).empty());
  }

  TEST("materials should be freed with the render system")
  {
    MockRenderer renderer;
    {
      AssetStore store;
      Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
      RenderSystem& system = scene.addSystem<RenderSystem>(renderer);
      MeshLibrary::Provider provider;
      provider.loadMaterial = [](std::string_view)
      { return std::make_shared<const MeshMaterial>(); };
      system.meshes().setProvider(provider);
      CHECK(system.meshes().material("wood").valid());
      CHECK(renderer.liveMaterials.size() == 1u);
    }
    CHECK(renderer.liveMaterials.empty());
  }

  return testing::report("renderSystemTests");
}
