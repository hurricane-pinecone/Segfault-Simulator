#include "../../testHarness.h"
#include "../rendering3d/mockRenderer3D.h"

#include <engine/core/components/cameraComponent.h>
#include <engine/core/components/grassBenderComponent.h>
#include <engine/core/components/grassComponent.h>
#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/parentComponent.h>
#include <engine/core/components/pointLightComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/reflection/reflection.h>
#include <engine/core/rendering/3d/grass.h>
#include <engine/core/rendering/3d/meshPrimitives.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering3d/renderSystem3D.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/serialization/sceneSerializer.h>
#include <engine/runtime/serialization/unresolvedComponents.h>

#include <glm/glm/gtc/quaternion.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sfs;
using sfs::testing3d::MockRenderer3D;

namespace
{
// A 3D scene with a named cube mesh, like an editor scene.
struct World
{
  AssetStore store;
  MockRenderer3D renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer3D = &renderer}};
  RenderSystem3D& render = scene.addSystem<RenderSystem3D>(renderer);
  MeshHandle cube = render.meshes().add(meshes::box({1, 1, 1}), "builtin:cube");

  SceneSerializer serializer()
  {
    return SceneSerializer(reflect::types(), &render.meshes());
  }

  Entity named(const std::string& name, glm::vec3 position = glm::vec3(0.0f))
  {
    Entity entity = scene.createEntity();
    entity.addComponent<NameComponent>(name);
    entity.addComponent<TransformComponent>(TransformComponent{position});
    return entity;
  }

  std::string nameOf(const Entity& entity)
  {
    return entity.isValid() ? entity.getComponent<NameComponent>().name : "";
  }

  Entity findByName(const std::string& name)
  {
    for (Entity entity : scene.entities())
      if (entity.hasComponent<NameComponent>() &&
          entity.getComponent<NameComponent>().name == name)
        return entity;
    return {};
  }
};
} // namespace

int main()
{
  TEST("a saved scene should load back with every component and field")
  {
    World source;
    Entity crate = source.scene.createEntity();
    crate.addComponent<NameComponent>("Crate");
    TransformComponent transform{{1.0f, 2.0f, 3.0f}};
    transform.setYaw(0.7f);
    transform.scale = {2.0f, 1.0f, 0.5f};
    crate.addComponent<TransformComponent>(transform);
    crate.addComponent<MeshRendererComponent>(
        MeshRendererComponent{source.cube, {1.0f, 0.5f, 0.25f, 1.0f}, false});

    Entity lamp = source.scene.createEntity();
    lamp.addComponent<NameComponent>("Lamp");
    lamp.addComponent<PointLightComponent>(PointLightComponent{
        {1.0f, 0.8f, 0.6f}, 2.5f, 12.0f, {0.0f, 0.0f, 1.8f}});

    const nlohmann::json saved = source.serializer().save(source.scene);

    World target;
    CHECK(target.serializer().load(saved, target.scene));
    CHECK(target.scene.entities().size() == 2u);

    Entity loaded = target.findByName("Crate");
    CHECK(loaded.isValid());
    const auto& t = loaded.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.z, 3.0f));
    CHECK(testing::approx(t.scale.x, 2.0f));
    CHECK(testing::approx(glm::angle(t.rotation), 0.7f, 1e-4f));

    const auto& r = loaded.getComponent<MeshRendererComponent>();
    CHECK(r.mesh == target.cube); // resolved by name in the new library
    CHECK(testing::approx(r.tint.g, 0.5f));
    CHECK(!r.castsShadow);

    const auto& light =
        target.findByName("Lamp").getComponent<PointLightComponent>();
    CHECK(testing::approx(light.intensity, 2.5f));
    CHECK(testing::approx(light.offset.z, 1.8f));
    CHECK(!target.findByName("Lamp").hasComponent<TransformComponent>());
  }

  TEST("render settings should round-trip with the scene")
  {
    World source;
    SceneLighting3D lighting;
    lighting.ambient = 0.2f;
    lighting.skyColor = {0.1f, 0.2f, 0.3f};
    source.render.setLighting(lighting);
    source.render.setShadowSettings({.enabled = false,
                                     .distance = 77.0f,
                                     .pointLights = 4,
                                     .pointResolution = 512});
    source.render.setWindSettings(
        {.direction = 200.0f, .strength = 0.8f, .gustiness = 0.1f});
    source.render.setHazeSettings({.strength = 0.4f, .distance = 300.0f});
    source.render.camera().yaw = 1.25f;
    source.render.camera().projection = Camera3D::Projection::Orthographic;

    World target;
    target.serializer().load(
        source.serializer().save(source.scene), target.scene);

    CHECK(testing::approx(target.render.lighting().ambient, 0.2f));
    CHECK(testing::approx(target.render.lighting().skyColor.b, 0.3f));
    CHECK(!target.render.shadowSettings().enabled);
    CHECK(testing::approx(target.render.shadowSettings().distance, 77.0f));
    CHECK(target.render.shadowSettings().pointLights == 4u);
    CHECK(target.render.shadowSettings().pointResolution == 512u);
    CHECK(testing::approx(target.render.windSettings().direction, 200.0f));
    CHECK(testing::approx(target.render.windSettings().strength, 0.8f));
    CHECK(testing::approx(target.render.windSettings().gustiness, 0.1f));
    CHECK(testing::approx(target.render.hazeSettings().strength, 0.4f));
    CHECK(testing::approx(target.render.hazeSettings().distance, 300.0f));
    CHECK(testing::approx(target.render.camera().yaw, 1.25f));
    CHECK(target.render.camera().projection ==
          Camera3D::Projection::Orthographic);
  }

  TEST("grass, with what's been painted, should load back as it was saved")
  {
    World source;
    Entity lawn = source.named("Lawn");
    lawn.addComponent<MeshRendererComponent>(
        MeshRendererComponent{source.cube});
    GrassComponent grass;
    grass.material = "asset:0123456789abcdef0123456789abcdef";
    grass.density = 120.0f;
    grass.stiffness = 0.8f;
    GrassMaps paint(GrassGrid::over({-0.5f, -0.5f}, {0.5f, 0.5f}, 0.1f));
    paint.fill(GrassMaps::Coverage, 0.0f);
    paint.stamp(GrassMaps::Coverage, {0.1f, 0.2f}, 0.3f, 1.0f);
    paint.stamp(GrassMaps::Data0, {-0.2f, 0.0f}, 0.2f, 0.6f);
    grass.paint = paint.toJson();
    lawn.addComponent<GrassComponent>(grass);
    Entity walker = source.named("Walker");
    walker.addComponent<GrassBenderComponent>(
        GrassBenderComponent{.radius = 1.25f});

    World target;
    target.serializer().load(
        source.serializer().save(source.scene), target.scene);
    const Entity loaded = target.findByName("Lawn");
    CHECK(loaded.isValid() && loaded.hasComponent<GrassComponent>());
    if (loaded.isValid() && loaded.hasComponent<GrassComponent>())
    {
      const GrassComponent& back = loaded.getComponent<GrassComponent>();
      CHECK(back.material == grass.material);
      CHECK(testing::approx(back.density, 120.0f));
      CHECK(testing::approx(back.stiffness, 0.8f));
      CHECK(GrassMaps::fromJson(back.paint) == paint);
    }
    const Entity bender = target.findByName("Walker");
    CHECK(bender.isValid() && bender.hasComponent<GrassBenderComponent>() &&
          testing::approx(
              bender.getComponent<GrassBenderComponent>().radius, 1.25f));
  }

  TEST("unknown components and missing fields should not stop a load")
  {
    const nlohmann::json data = {
        {"formatVersion", 1},
        {"entities",
         {{{"NameComponent", {{"name", "Survivor"}}},
           {"RemovedComponent", {{"x", 1}}},
           {"TransformComponent", {{"position", {4.0, 5.0, 6.0}}}}}}}};

    World world;
    CHECK(world.serializer().load(data, world.scene));
    Entity survivor = world.findByName("Survivor");
    CHECK(survivor.isValid());
    const auto& t = survivor.getComponent<TransformComponent>();
    CHECK(testing::approx(t.position.x, 4.0f));
    CHECK(testing::approx(t.scale.x, 1.0f)); // missing field kept its default
  }

  TEST("unknown components should be kept and saved back unchanged")
  {
    const nlohmann::json data = {
        {"formatVersion", 1},
        {"entities",
         {{{"NameComponent", {{"name", "Field"}}},
           {"CropComponent", {{"growth", 0.75}, {"variety", "wheat"}}}}}}};

    World world;
    world.serializer().load(data, world.scene);
    const nlohmann::json saved = world.serializer().save(world.scene);

    const nlohmann::json& entity = saved["entities"][0];
    CHECK(entity.contains("CropComponent"));
    CHECK(entity["CropComponent"]["variety"] == "wheat");
    CHECK(entity["CropComponent"]["growth"] == 0.75);
    CHECK(entity.contains("NameComponent"));
  }

  TEST("a wrongly typed field should keep its default")
  {
    const nlohmann::json data = {
        {"formatVersion", 1},
        {"entities", {{{"PointLightComponent", {{"intensity", "bright"}}}}}}};

    World world;
    world.serializer().load(data, world.scene);
    const auto entities = world.scene.entities();
    CHECK(entities.size() == 1u);
    CHECK(testing::approx(
        entities[0].getComponent<PointLightComponent>().intensity, 1.0f));
  }

  TEST("an unknown mesh name should survive a save, drawing nothing meanwhile")
  {
    const nlohmann::json data = {
        {"formatVersion", 1},
        {"entities",
         {{{"MeshRendererComponent", {{"mesh", "mygame:tower"}}}}}}};

    World world;
    world.serializer().load(data, world.scene);
    const MeshHandle mesh =
        world.scene.entities()[0].getComponent<MeshRendererComponent>().mesh;
    CHECK(world.render.meshes().isMissing(mesh));
    CHECK(world.render.meshes().bounds(mesh) ==
          nullptr); // skipped when drawing
    CHECK(world.render.meshes().missing("mygame:tower") ==
          mesh); // one stand-in per name

    const nlohmann::json saved = world.serializer().save(world.scene);
    CHECK(saved["entities"][0]["MeshRendererComponent"]["mesh"] ==
          "mygame:tower");

    // Expected missing (its game module isn't built yet): the same, quietly.
    World waiting;
    waiting.render.meshes().setMissingExpected(true);
    waiting.serializer().load(data, waiting.scene);
    CHECK(waiting.render.meshes().isMissing(
        waiting.scene.entities()[0]
            .getComponent<MeshRendererComponent>()
            .mesh));
    CHECK(waiting.serializer().save(waiting.scene) == saved);
  }

  TEST("files from a newer format or without entities should be rejected")
  {
    World world;
    CHECK(!world.serializer().load(
        {{"formatVersion", 99}, {"entities", nlohmann::json::array()}},
        world.scene));
    CHECK(!world.serializer().load({{"formatVersion", 1}}, world.scene));
    CHECK(world.scene.entities().empty());
  }

  TEST("save, load, save should give an identical file in the same order")
  {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "sfsSceneSerializerTests";
    std::filesystem::create_directories(dir);
    const auto readAll = [](const std::filesystem::path& path)
    {
      std::ifstream in(path);
      std::stringstream text;
      text << in.rdbuf();
      return text.str();
    };

    World source;
    for (const char* name : {"First", "Second", "Third"})
    {
      Entity entity = source.scene.createEntity();
      entity.addComponent<NameComponent>(name);
      entity.addComponent<TransformComponent>(
          TransformComponent{{0.45f, 0.1f, -2.3f}});
    }
    CHECK(source.serializer().saveFile(source.scene, dir / "a.scene"));

    // Clear and reload into the same scene, as the editor does.
    for (Entity entity : source.scene.entities())
      source.scene.destroyEntity(entity);
    source.scene.flushEntities();
    CHECK(source.serializer().loadFile(dir / "a.scene", source.scene));
    source.scene.flushEntities();
    CHECK(source.serializer().saveFile(source.scene, dir / "b.scene"));

    const std::string a = readAll(dir / "a.scene");
    CHECK(a == readAll(dir / "b.scene"));
    CHECK(a.find("0.45") != std::string::npos);              // clean decimals
    CHECK(a.find("0.449999") == std::string::npos);          // no widened noise
    CHECK(a.find("[0.45, 0.1, -2.3]") != std::string::npos); // vectors inline
    CHECK(a.find("First") < a.find("Second"));
    CHECK(a.find("Second") < a.find("Third"));

    std::filesystem::remove_all(dir);
  }

  TEST("a file saved before the components were shared should still load")
  {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "sfsSceneSerializerLegacy";
    std::filesystem::create_directories(dir);
    const nlohmann::json legacy = {
        {"formatVersion", 1},
        {"entities",
         {{{"Transform3DComponent", {{"position", {1.0, 2.0, 3.0}}}},
           {"PointLight3DComponent", {{"radius", 4.0}}},
           {"Camera3DComponent", {{"fieldOfView", 60.0}}},
           {"children",
            {{{"Transform3DComponent", {{"position", {0.0, 0.0, 5.0}}}}}}}}}}};
    std::ofstream(dir / "old.scene") << legacy.dump();

    World world;
    CHECK(world.serializer().loadFile(dir / "old.scene", world.scene));
    world.scene.flushEntities();

    const auto entities = world.scene.entities();
    CHECK(entities.size() == 2u);
    const Entity root = entities[0];
    CHECK(testing::approx(
        root.getComponent<TransformComponent>().position.z, 3.0f));
    CHECK(
        testing::approx(root.getComponent<PointLightComponent>().radius, 4.0f));
    CHECK(testing::approx(
        root.getComponent<CameraComponent>().fieldOfView, 60.0f));
    CHECK(!root.hasComponent<UnresolvedComponents>());
    CHECK(testing::approx(
        entities[1].getComponent<TransformComponent>().position.z, 5.0f));

    std::filesystem::remove_all(dir);
  }

  TEST("a hierarchy should save nested and load back with the same parents")
  {
    World source;
    Entity farm = source.named("Farm", {10.0f, 0.0f, 0.0f});
    Entity loose = source.named("Loose");
    Entity barn = source.named("Barn", {1.0f, 0.0f, 0.0f});
    Entity door = source.named("Door", {0.0f, 0.0f, 2.0f});
    hierarchy::setParent(barn, farm);
    hierarchy::setParent(door, barn);
    source.scene.flushEntities();

    const nlohmann::json saved = source.serializer().save(source.scene);
    CHECK(saved["formatVersion"] == 2);
    CHECK(saved["entities"].size() == 2u); // roots: Farm, Loose
    CHECK(saved["entities"][0]["children"][0]["NameComponent"]["name"] ==
          "Barn");
    CHECK(saved["entities"][0]["children"][0]["children"][0]["NameComponent"]
               ["name"] == "Door");
    CHECK(!saved["entities"][1].contains("children"));
    CHECK(!saved.dump().contains("ParentComponent"));

    World target;
    CHECK(target.serializer().load(saved, target.scene));
    target.scene.flushEntities();
    CHECK(target.scene.entities().size() == 4u);
    CHECK(target.nameOf(hierarchy::parentOf(target.findByName("Door"))) ==
          "Barn");
    CHECK(target.nameOf(hierarchy::parentOf(target.findByName("Barn"))) ==
          "Farm");
    CHECK(!hierarchy::parentOf(target.findByName("Loose")).isValid());
    const glm::vec3 doorAt =
        hierarchy::worldPosition(target.findByName("Door"));
    CHECK(testing::approx(doorAt.x, 11.0f));
    CHECK(testing::approx(doorAt.z, 2.0f));
    (void)loose;
  }

  TEST("loading should create entities parents first, so a reload keeps that "
       "order")
  {
    World world;
    Entity a = world.named("A");
    Entity b = world.named("B");
    Entity c = world.named("C");
    hierarchy::setParent(a, c); // saved order: B, C, A
    world.scene.flushEntities();
    const nlohmann::json saved = world.serializer().save(world.scene);
    (void)b;

    for (Entity entity : world.scene.entities())
      world.scene.destroyEntity(entity);
    world.scene.flushEntities();
    CHECK(world.serializer().load(saved, world.scene));
    world.scene.flushEntities();

    std::vector<std::string> names;
    for (const Entity& entity : world.scene.entities())
      names.push_back(world.nameOf(entity));
    CHECK((names == std::vector<std::string>{"B", "C", "A"}));
    CHECK(world.serializer().save(world.scene) == saved);
  }

  TEST("saveEntity and loadEntity should copy a whole branch under a given "
       "parent")
  {
    World world;
    Entity root = world.named("Root");
    Entity lamp = world.named("Lamp", {0.0f, 0.0f, 3.0f});
    Entity bulb = world.named("Bulb");
    hierarchy::setParent(lamp, root);
    hierarchy::setParent(bulb, lamp);
    world.scene.flushEntities();

    const Entity copy = world.serializer().loadEntity(
        world.serializer().saveEntity(world.scene, lamp), world.scene, root);
    world.scene.flushEntities();

    CHECK(world.scene.entities().size() == 5u);
    CHECK(hierarchy::parentOf(copy) == root);
    const hierarchy::EntityTree tree(world.scene.entities());
    CHECK(tree.children(root).size() == 2u);
    CHECK(tree.children(copy).size() == 1u);
    CHECK(world.nameOf(tree.children(copy)[0]) == "Bulb");
  }

  TEST("a format 1 file (no hierarchy) should still load")
  {
    World world;
    const nlohmann::json v1 = {
        {"formatVersion", 1},
        {"entities", {{{"NameComponent", {{"name", "Old"}}}}}},
    };
    CHECK(world.serializer().load(v1, world.scene));
    world.scene.flushEntities();
    CHECK(world.findByName("Old").isValid());
    CHECK(!world.findByName("Old").hasComponent<ParentComponent>());
  }

  return testing::report("sceneSerializerTests");
}
