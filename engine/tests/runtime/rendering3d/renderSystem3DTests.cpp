#include "../../testHarness.h"
#include "mockRenderer3D.h"

#include <engine/core/components/boxCollider3DComponent.h>
#include <engine/core/components/camera3DComponent.h>
#include <engine/core/components/grassBenderComponent.h>
#include <engine/core/components/grassComponent.h>
#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/pointLight3DComponent.h>
#include <engine/core/components/rigidBody3DComponent.h>
#include <engine/core/components/sphereCollider3DComponent.h>
#include <engine/core/components/transform3DComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/rendering/3d/materialGraphCompiler.h>
#include <engine/core/rendering/3d/meshPrimitives.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/input/input.h>
#include <engine/runtime/rendering3d/iRenderer3D.h>
#include <engine/runtime/rendering3d/meshLibrary.h>
#include <engine/runtime/rendering3d/modules/grassModule.h>
#include <engine/runtime/rendering3d/modules/meshModule.h>
#include <engine/runtime/rendering3d/modules/uiModule.h>
#include <engine/runtime/rendering3d/renderSystem3D.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/ui/layer.h>

#include <algorithm>
#include <vector>

using namespace sfs;
using sfs::testing3d::MockRenderer3D;

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
    lamp.addComponent<Transform3DComponent>(
        Transform3DComponent{{4.0f, 5.0f, 0.0f}});
    lamp.addComponent<PointLight3DComponent>(
        PointLight3DComponent{{1, 1, 1}, 1.0f, 6.0f, {0.0f, 0.0f, 2.0f}});

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
    cart.addComponent<Transform3DComponent>(
        Transform3DComponent{{2.0f, 0.0f, 0.0f}});
    Entity crate = h.spawn(box, {0.0f, 1.0f, 0.0f});
    hierarchy::setParent(crate, cart);
    Entity lantern = h.scene.createEntity();
    lantern.addComponent<Transform3DComponent>(
        Transform3DComponent{{0.0f, 0.0f, 1.0f}});
    lantern.addComponent<PointLight3DComponent>();
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
    off.addComponent<Transform3DComponent>(
        Transform3DComponent{{0.0f, 0.0f, 50.0f}});
    off.addComponent<Camera3DComponent>()
        .getComponent<Camera3DComponent>()
        .active = false;

    Entity rig = h.scene.createEntity();
    rig.addComponent<Transform3DComponent>(
        Transform3DComponent{{-5.0f, 2.0f, 3.0f}});
    Entity eye = h.scene.createEntity();
    Transform3DComponent eyeTransform{{0.0f, 0.0f, 1.0f}};
    eyeTransform.setYaw(1.5707964f); // facing +Y
    eye.addComponent<Transform3DComponent>(eyeTransform);
    Camera3DComponent lens;
    lens.orthographic = true;
    lens.orthoHeight = 9.0f;
    lens.focusDistance = 4.0f;
    eye.addComponent<Camera3DComponent>(lens);
    hierarchy::setParent(eye, rig);

    h.frame();

    CHECK(h.system.sceneCamera() == eye);
    const Camera3D& camera = h.system.camera();
    CHECK(testing::approx(camera.position().x, -5.0f));
    CHECK(testing::approx(camera.position().y, 2.0f));
    CHECK(testing::approx(camera.position().z, 4.0f));
    CHECK(testing::approx(camera.target.y, 6.0f)); // 4 ahead along +Y
    CHECK(camera.projection == Camera3D::Projection::Orthographic);
    CHECK(testing::approx(camera.orthoHeight, 9.0f));
  }

  TEST("not following the scene camera should leave camera() to its driver")
  {
    Harness h;
    Entity eye = h.scene.createEntity();
    eye.addComponent<Transform3DComponent>(
        Transform3DComponent{{40.0f, 0.0f, 0.0f}});
    eye.addComponent<Camera3DComponent>();
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
    const Ray3D ray = h.system.screenRay(glm::vec2(size) * 0.5f);
    const glm::vec3 toTarget =
        glm::normalize(h.system.camera().target - ray.origin);
    CHECK(testing::approx(glm::dot(ray.direction, toTarget), 1.0f, 1e-3f));
  }

  TEST("screenRay should use the size the scene was drawn at, not a later one")
  {
    Harness h;
    h.frame();
    const glm::vec2 drawn = glm::vec2(h.renderer.viewportSize());
    const Ray3D before = h.system.screenRay(drawn * 0.5f);
    h.renderer.viewport =
        h.renderer.viewport * 3; // e.g. the window, between frames
    const Ray3D after = h.system.screenRay(drawn * 0.5f);
    CHECK(testing::approx(
        glm::dot(before.direction, after.direction), 1.0f, 1e-5f));
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
      const PointLight3D& light = h.renderer.lights[0];
      CHECK(glm::length(light.position - glm::vec3(2.0f, 3.0f, 0.5f)) < 0.05f);
      CHECK(testing::approx(light.color.g, 0.5f, 1e-3f));
      CHECK(testing::approx(light.intensity, 4.0f, 1e-3f));
    }

    // Twice the size: four times the glowing area, so it reaches twice as far.
    const float radius =
        h.renderer.lights.empty() ? 0.0f : h.renderer.lights[0].radius;
    lamp.getComponent<Transform3DComponent>().scale = glm::vec3(2.0f);
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
    const PointLight3D normal =
        h.renderer.lights.empty() ? PointLight3D{} : h.renderer.lights[0];
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
    camera.addComponent<Transform3DComponent>();
    Camera3DComponent lens;
    lens.bloomIntensity = 1.7f;
    lens.bloomThreshold = 2.5f;
    camera.addComponent<Camera3DComponent>(lens);
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
    for (const PointLight3D& light : h.renderer.lights)
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
    ShadowSettings3D shadows;
    shadows.pointLights = 2;
    h.system.setShadowSettings(shadows);
    // Five lights along y through the target, one off behind the camera, and
    // one that doesn't cast.
    for (int i = 0; i < 5; ++i)
    {
      Entity lamp = h.scene.createEntity();
      lamp.addComponent<Transform3DComponent>(
          Transform3DComponent{{0.0f, static_cast<float>(i) * 3.0f, 0.0f}});
      lamp.addComponent<PointLight3DComponent>(
          PointLight3DComponent{glm::vec3(1.0f), 1.0f, 2.0f});
    }
    Entity behind = h.scene.createEntity();
    behind.addComponent<Transform3DComponent>(
        Transform3DComponent{{-60.0f, 0.0f, 0.0f}});
    behind.addComponent<PointLight3DComponent>(
        PointLight3DComponent{glm::vec3(1.0f), 1.0f, 2.0f});
    Entity plain = h.scene.createEntity();
    plain.addComponent<Transform3DComponent>(
        Transform3DComponent{{0.0f, -1.0f, 0.0f}});
    PointLight3DComponent noShadow{glm::vec3(1.0f), 1.0f, 2.0f};
    noShadow.castsShadow = false;
    plain.addComponent<PointLight3DComponent>(noShadow);

    // A box by the first light, and one well away from every light.
    const MeshHandle box = h.system.meshes().add(meshes::box({1, 1, 1}));
    h.spawn(box, {0.0f, 1.0f, 0.0f});
    h.spawn(box, {0.0f, 30.0f, 0.0f});

    h.frame();

    std::vector<float> shadowed;
    for (const PointLight3D& light : h.renderer.lights)
      if (light.castsShadow)
        shadowed.push_back(light.position.y);
    std::sort(shadowed.begin(), shadowed.end());
    CHECK(shadowed == std::vector<float>({0.0f, 3.0f}));
    CHECK(h.system.stats().shadowedLights == 2u);
    // Only the box near a shadowed light casts into its shadow map.
    std::size_t casters = 0;
    for (const MockRenderer3D::Batch& batch : h.renderer.batches)
      casters += batch.lightShadowInstances;
    CHECK(casters == 1u);

    shadows.pointLights = 0;
    h.system.setShadowSettings(shadows);
    h.frame();
    CHECK(std::none_of(h.renderer.lights.begin(),
                       h.renderer.lights.end(),
                       [](const PointLight3D& light)
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
    lawn.getComponent<Transform3DComponent>().position = {10.0f, 0.0f, 0.0f};
    h.frame();
    CHECK(h.renderer.liveGrass.size() == 1u &&
          h.renderer.liveGrass.begin()->first == first);
    CHECK(!h.renderer.grassDrawn.empty() &&
          h.renderer.grassDrawn[0].shape.density == 40.0f &&
          h.renderer.grassDrawn[0].model[3].x == 10.0f);
    CHECK(h.renderer.mapUpdates.size() == 1u); // nothing new

    // Scaled: built again (more texels), the old one freed.
    lawn.getComponent<Transform3DComponent>().scale = {2.0f, 2.0f, 1.0f};
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
    walker.addComponent<Transform3DComponent>(
        Transform3DComponent{{0.0f, -3.0f, 0.4f}});
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
    ball.getComponent<Transform3DComponent>().position = {-3.0f, -3.0f, 5.0f};
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
    wheel.getComponent<Transform3DComponent>().scale = glm::vec3(1.5f);
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
      return testing::report("renderSystem3DTests");
    const glm::vec2 on(button->rect.x + button->rect.width * 0.5f,
                       button->rect.y + button->rect.height * 0.5f);

    // The scene's input, the UI first (as Scene3D does).
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
    eye.addComponent<Transform3DComponent>();
    Camera3DComponent lens;
    lens.depthOfField = true;
    lens.focusDistance = 7.0f;
    lens.focusRange = 3.0f;
    lens.focusBlur = 2.0f;
    eye.addComponent<Camera3DComponent>(lens);
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
    MockRenderer3D renderer;
    {
      AssetStore store;
      Scene scene{
          0, SceneServices{.assetStore = store, .renderer3D = &renderer}};
      auto& system = scene.addSystem<RenderSystem3D>(renderer);
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
    MockRenderer3D renderer;
    {
      AssetStore store;
      Scene scene{
          0, SceneServices{.assetStore = store, .renderer3D = &renderer}};
      RenderSystem3D& system = scene.addSystem<RenderSystem3D>(renderer);
      MeshLibrary::Provider provider;
      provider.loadMaterial = [](std::string_view)
      { return std::make_shared<const MeshMaterial>(); };
      system.meshes().setProvider(provider);
      CHECK(system.meshes().material("wood").valid());
      CHECK(renderer.liveMaterials.size() == 1u);
    }
    CHECK(renderer.liveMaterials.empty());
  }

  return testing::report("renderSystem3DTests");
}
