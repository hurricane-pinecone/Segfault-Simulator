#include "../../testHarness.h"
#include "../rendering3d/mockRenderer3D.h"

#include <engine/core/components/meshRendererComponent.h>
#include <engine/core/components/nameComponent.h>
#include <engine/core/components/pointLight3DComponent.h>
#include <engine/core/components/transform3DComponent.h>
#include <engine/core/rendering/3d/meshPrimitives.h>
#include <engine/core/reflection/reflection.h>
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

  SceneSerializer serializer() { return SceneSerializer(reflect::types(), &render.meshes()); }

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
    Transform3DComponent transform{{1.0f, 2.0f, 3.0f}};
    transform.setYaw(0.7f);
    transform.scale = {2.0f, 1.0f, 0.5f};
    crate.addComponent<Transform3DComponent>(transform);
    crate.addComponent<MeshRendererComponent>(
        MeshRendererComponent{source.cube, {1.0f, 0.5f, 0.25f, 1.0f}, false});

    Entity lamp = source.scene.createEntity();
    lamp.addComponent<NameComponent>("Lamp");
    lamp.addComponent<PointLight3DComponent>(
        PointLight3DComponent{{1.0f, 0.8f, 0.6f}, 2.5f, 12.0f, {0.0f, 0.0f, 1.8f}});

    const nlohmann::json saved = source.serializer().save(source.scene);

    World target;
    CHECK(target.serializer().load(saved, target.scene));
    CHECK(target.scene.entities().size() == 2u);

    Entity loaded = target.findByName("Crate");
    CHECK(loaded.isValid());
    const auto& t = loaded.getComponent<Transform3DComponent>();
    CHECK(testing::approx(t.position.z, 3.0f));
    CHECK(testing::approx(t.scale.x, 2.0f));
    CHECK(testing::approx(glm::angle(t.rotation), 0.7f, 1e-4f));

    const auto& r = loaded.getComponent<MeshRendererComponent>();
    CHECK(r.mesh == target.cube); // resolved by name in the new library
    CHECK(testing::approx(r.tint.g, 0.5f));
    CHECK(!r.castsShadow);

    const auto& light = target.findByName("Lamp").getComponent<PointLight3DComponent>();
    CHECK(testing::approx(light.intensity, 2.5f));
    CHECK(testing::approx(light.offset.z, 1.8f));
    CHECK(!target.findByName("Lamp").hasComponent<Transform3DComponent>());
  }

  TEST("render settings should round-trip with the scene")
  {
    World source;
    SceneLighting3D lighting;
    lighting.ambient = 0.2f;
    lighting.skyColor = {0.1f, 0.2f, 0.3f};
    source.render.setLighting(lighting);
    source.render.setShadowSettings({.enabled = false, .distance = 77.0f});
    source.render.camera().yaw = 1.25f;
    source.render.camera().projection = Camera3D::Projection::Orthographic;

    World target;
    target.serializer().load(source.serializer().save(source.scene), target.scene);

    CHECK(testing::approx(target.render.lighting().ambient, 0.2f));
    CHECK(testing::approx(target.render.lighting().skyColor.b, 0.3f));
    CHECK(!target.render.shadowSettings().enabled);
    CHECK(testing::approx(target.render.shadowSettings().distance, 77.0f));
    CHECK(testing::approx(target.render.camera().yaw, 1.25f));
    CHECK(target.render.camera().projection == Camera3D::Projection::Orthographic);
  }

  TEST("unknown components and missing fields should not stop a load")
  {
    const nlohmann::json data = {
        {"formatVersion", 1},
        {"entities",
         {{{"NameComponent", {{"name", "Survivor"}}},
           {"RemovedComponent", {{"x", 1}}},
           {"Transform3DComponent", {{"position", {4.0, 5.0, 6.0}}}}}}}};

    World world;
    CHECK(world.serializer().load(data, world.scene));
    Entity survivor = world.findByName("Survivor");
    CHECK(survivor.isValid());
    const auto& t = survivor.getComponent<Transform3DComponent>();
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
        {"entities", {{{"PointLight3DComponent", {{"intensity", "bright"}}}}}}};

    World world;
    world.serializer().load(data, world.scene);
    const auto entities = world.scene.entities();
    CHECK(entities.size() == 1u);
    CHECK(testing::approx(
        entities[0].getComponent<PointLight3DComponent>().intensity, 1.0f));
  }

  TEST("an unknown mesh name should survive a save, drawing nothing meanwhile")
  {
    const nlohmann::json data = {
        {"formatVersion", 1},
        {"entities", {{{"MeshRendererComponent", {{"mesh", "mygame:tower"}}}}}}};

    World world;
    world.serializer().load(data, world.scene);
    const MeshHandle mesh = world.scene.entities()[0].getComponent<MeshRendererComponent>().mesh;
    CHECK(world.render.meshes().isMissing(mesh));
    CHECK(world.render.meshes().bounds(mesh) == nullptr); // skipped when drawing
    CHECK(world.render.meshes().missing("mygame:tower") == mesh); // one stand-in per name

    const nlohmann::json saved = world.serializer().save(world.scene);
    CHECK(saved["entities"][0]["MeshRendererComponent"]["mesh"] == "mygame:tower");
  }

  TEST("files from a newer format or without entities should be rejected")
  {
    World world;
    CHECK(!world.serializer().load({{"formatVersion", 99}, {"entities", nlohmann::json::array()}},
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
      entity.addComponent<Transform3DComponent>(Transform3DComponent{{0.45f, 0.1f, -2.3f}});
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
    CHECK(a.find("0.45") != std::string::npos);           // clean decimals
    CHECK(a.find("0.449999") == std::string::npos);       // no widened noise
    CHECK(a.find("[0.45, 0.1, -2.3]") != std::string::npos); // vectors inline
    CHECK(a.find("First") < a.find("Second"));
    CHECK(a.find("Second") < a.find("Third"));

    std::filesystem::remove_all(dir);
  }

  return testing::report("sceneSerializerTests");
}
