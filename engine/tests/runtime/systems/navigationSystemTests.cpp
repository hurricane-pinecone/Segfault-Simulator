#include "../../testHarness.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/core/components/boxCollider3DComponent.h>
#include <engine/core/components/navAgentComponent.h>
#include <engine/core/components/rigidBody3DComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/components/vehicleComponent.h>
#include <engine/core/hierarchy/hierarchy.h>
#include <engine/core/reflection/registration.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/navigation/vehicle/vehicleKind.h>
#include <engine/runtime/navigation/vehicle/vehicleSpec.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/scriptableObjects/scriptableObjects.h>
#include <engine/runtime/scripting/scriptRegistry.h>
#include <engine/runtime/systems/navigationSystem.h>
#include <engine/runtime/systems/physicsSystem3D.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace sfs;

namespace
{
namespace fs = std::filesystem;
constexpr double kFrame = 1.0 / 30.0;

// A game's extension of the spec (as CozyFarm's TractorSpec), registered as
// its generated code would.
struct TestTractorSpec : VehicleSpec
{
  float hitchHeight = 0.5f;
};

void registerTractorSpec()
{
  using namespace reflect;
  TypeInfo type = derivedType<TestTractorSpec>(
      "TestTractorSpec", "Test Tractor Spec", "navTests", "VehicleSpec");
  FieldInfo field;
  field.name = "hitchHeight";
  field.kind = FieldKind::Float;
  field.address = [](void* o) -> void*
  { return &objectAs<TestTractorSpec>(o)->hitchHeight; };
  type.fields.push_back(field);
  types().add(type);
}

// The project's vehicle specs: a car (a VehicleSpec) and a tractor (a
// TestTractorSpec), as .asset files the process-wide assets read.
struct Specs
{
  fs::path root = fs::temp_directory_path() / "sfs_nav_specs";
  std::unique_ptr<AssetDatabase> catalogue;
  std::string car, tractor;

  Specs()
  {
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    write("Car",
          "VehicleSpec",
          {{"wheelbase", 2.7f},
           {"maxSteerAngle", 28.4f},
           {"topSpeed", 6.0f},
           {"roadSpeed", 6.0f},
           {"mass", 1400.0f},
           {"power", 60.0f}});
    write("Tractor",
          "TestTractorSpec",
          {{"wheelbase", 2.7f},
           {"maxSteerAngle", 28.4f},
           {"topSpeed", 6.0f},
           {"roadSpeed", 6.0f},
           {"mass", 6000.0f},
           {"power", 90.0f},
           {"hitchHeight", 0.8f}});
    catalogue = std::make_unique<AssetDatabase>(
        root, AssetDatabase::Mode::ReadWrite, std::vector<fs::path>{"assets"});
    scriptableObjects().setCatalogue(catalogue.get());
    car = refOf("Car");
    tractor = refOf("Tractor");
  }
  ~Specs()
  {
    scriptableObjects().setCatalogue(nullptr);
    catalogue.reset();
    fs::remove_all(root);
  }
  void write(const std::string& name,
             const std::string& type,
             const nlohmann::json& fields)
  {
    std::ofstream(root / "assets" / (name + ".asset"))
        << ScriptableObjects::fileContents(type, fields).dump(2);
  }
  std::string refOf(const std::string& name) const
  {
    return "asset:" +
           catalogue->findByPath("assets/" + name + ".asset")->id.toString();
  }
};

struct World
{
  AssetStore store;
  Scene scene{0, SceneServices{.assetStore = store}};
  PhysicsSystem3D& physics = scene.addSystem<PhysicsSystem3D>();
  NavigationSystem& navigation = scene.addSystem<NavigationSystem>();

  Entity solid(const glm::vec3& position, const glm::vec3& size)
  {
    Entity e = scene.createEntity();
    e.addComponent<TransformComponent>(TransformComponent{position});
    e.addComponent<BoxCollider3DComponent>(BoxCollider3DComponent{size});
    return e;
  }

  // A car-sized agent, its rear axle at its origin, of the spec `spec`
  // ("asset:<id>"; empty: no VehicleComponent).
  Entity car(const glm::vec2& at, float yaw, const std::string& spec)
  {
    Entity e = scene.createEntity();
    TransformComponent t{glm::vec3(at, 0.0f)};
    t.setYaw(yaw);
    e.addComponent<TransformComponent>(t);
    e.addComponent<BoxCollider3DComponent>(
        BoxCollider3DComponent{{4.6f, 1.9f, 1.6f}, {1.3f, 0.0f, 0.8f}});
    RigidBody3DComponent body;
    body.kinematic = true;
    e.addComponent<RigidBody3DComponent>(body);
    e.addComponent<NavAgentComponent>(NavAgentComponent{});
    if (!spec.empty())
    {
      VehicleComponent vehicle;
      vehicle.spec.asset = spec;
      e.addComponent<VehicleComponent>(vehicle);
    }
    return e;
  }

  // Run till `agent` arrives or is blocked (or `seconds` pass).
  std::vector<NavigationSystem::Event> run(const Entity& agent, double seconds)
  {
    std::vector<NavigationSystem::Event> events;
    for (double t = 0.0; t < seconds; t += kFrame)
    {
      scene.update(kFrame);
      events.insert(
          events.end(), navigation.events().begin(), navigation.events().end());
      const nav::Fleet::State st = navigation.state(agent);
      // (Plans run on worker threads: while it plans, wait for real.)
      if (st == nav::Fleet::State::Planning)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (st == nav::Fleet::State::Arrived || st == nav::Fleet::State::Blocked)
        break;
    }
    return events;
  }
};
} // namespace

int main()
{
  registerTractorSpec();

  TEST("VehicleSpec should be reflected as the engine's, and a game's spec "
       "derived from it should have its fields first")
  {
    const reflect::TypeInfo* spec = reflect::types().find("VehicleSpec");
    CHECK(spec && spec->kind == reflect::TypeKind::ScriptableObject &&
          spec->module == "engine");
    const reflect::TypeInfo* tractor = reflect::types().find("TestTractorSpec");
    CHECK(tractor && spec && tractor->fields.size() == spec->fields.size() + 1);
    CHECK(tractor && tractor->fields.front().name == "wheelbase" &&
          tractor->fields.back().name == "hitchHeight");
    CHECK(reflect::types().isA("TestTractorSpec", "VehicleSpec"));
  }

  TEST("a scene should be able to list NavigationSystem by name")
  {
    const ScriptType* type = scripts().find("NavigationSystem");
    CHECK(type != nullptr);
    CHECK(type && type->kind == ScriptType::Kind::System);
  }

  TEST("an agent sent past a wall should drive round it and arrive, its "
       "transform moved by the system")
  {
    Specs specs;
    World world;
    world.solid({12.0f, 0.0f, 1.0f}, {1.0f, 8.0f, 2.0f});
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    world.scene.update(kFrame); // (the physics picks up its bodies)
    world.navigation.requestGoal(car, {{{24.0f, 0.0f}, 0.0f}});
    const auto events = world.run(car, 90.0);
    CHECK(world.navigation.state(car) == nav::Fleet::State::Arrived);
    const glm::vec3 at = hierarchy::worldPosition(car);
    CHECK(glm::length(glm::vec2(at) - glm::vec2(24.0f, 0.0f)) < 0.1f);
    bool arrived = false;
    for (const auto& e : events)
      arrived = arrived || (e.agent == car &&
                            e.what.kind == nav::Fleet::Event::Kind::Arrived);
    CHECK(arrived);
  }

  TEST("a vehicle agent should drive by a derived spec (a game's "
       "TractorSpec) through its VehicleComponent, and one without a "
       "VehicleComponent should be left out")
  {
    Specs specs;
    World world;
    Entity tractor = world.car({0.0f, 0.0f}, 0.0f, specs.tractor);
    Entity bare = world.car({0.0f, 10.0f}, 0.0f, {});
    world.scene.update(kFrame);
    AssetRef<VehicleSpec> ref;
    ref.asset = specs.tractor;
    CHECK(dynamic_cast<const TestTractorSpec*>(ref.get()) != nullptr);
    CHECK(scriptableObjects().list("VehicleSpec").size() == 2u);
    world.navigation.requestGoal(tractor, {{{15.0f, 0.0f}, 0.0f}});
    world.navigation.requestGoal(bare, {{{15.0f, 10.0f}, 0.0f}});
    world.run(tractor, 60.0);
    CHECK(world.navigation.state(tractor) == nav::Fleet::State::Arrived);
    CHECK(world.navigation.state(bare) != nav::Fleet::State::Arrived);
    const glm::vec3 at = hierarchy::worldPosition(bare);
    CHECK(glm::length(glm::vec2(at) - glm::vec2(0.0f, 10.0f)) < 1.0e-3f);
  }

  TEST("towing and loading should slow its acceleration and braking, the "
       "grip its cornering: derived, never stored")
  {
    VehicleSpec spec;
    spec.mass = 5000.0f;
    spec.power = 75.0f;
    spec.brakeForce = 25.0f;
    nav::AgentSpec alone, towing, slippery;
    nav::vehicle::Rig rig;
    rig.effort = 1.0f;
    nav::vehicle::applySpec(alone, spec, rig);
    rig.extraMass = 5000.0f;
    nav::vehicle::applySpec(towing, spec, rig);
    rig.extraMass = 0.0f;
    rig.grip = 0.2f;
    nav::vehicle::applySpec(slippery, spec, rig);
    CHECK(alone.accel > 0.0f && towing.accel < alone.accel * 0.6f);
    CHECK(towing.brake < alone.brake * 0.6f);
    CHECK(slippery.lateralAccel < alone.lateralAccel * 0.5f);
    CHECK(std::abs(alone.minRadius - 2.6f / std::tan(glm::radians(27.5f))) <
          1.0e-3f);
  }

  TEST("its debug toggles should be offered as settings")
  {
    World world;
    bool paths = false;
    for (const ModuleSetting& s : world.navigation.settings())
      paths = paths || s.label == "Draw paths";
    CHECK(paths);
  }

  TEST("a mounted implement (rigid hitch) should join the agent's boxes; one "
       "left out (setActive false) should leave the fleet till it's back")
  {
    Specs specs;
    World world;
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    Entity tool = world.solid({-2.0f, 0.0f, 0.5f}, {1.0f, 3.0f, 1.0f});
    world.scene.update(kFrame);
    const auto id = static_cast<nav::Fleet::AgentId>(car.getId());
    const std::size_t alone = world.navigation.fleet().spec(id)->boxes.size();
    NavigationSystem::Hitch mounted;
    mounted.towed = tool;
    mounted.rigid = true;
    world.navigation.hitch(car, mounted);
    world.scene.update(kFrame);
    const nav::AgentSpec* spec = world.navigation.fleet().spec(id);
    CHECK(spec->boxes.size() == alone + 1);
    CHECK(!spec->trailer);
    world.navigation.setActive(car, false);
    world.scene.update(kFrame);
    CHECK(!world.navigation.fleet().has(id));
    world.navigation.setActive(car, true);
    world.scene.update(kFrame);
    CHECK(world.navigation.fleet().has(id));
  }

  return testing::report("navigationSystemTests");
}
