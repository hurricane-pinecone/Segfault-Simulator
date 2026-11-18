#include "../../testHarness.h"
#include "engine/core/assets/assetDatabase.h"
#include "engine/core/assets/scriptableObject.h"
#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/navAgentComponent.h"
#include "engine/core/components/rigidBody3DComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/components/vehicleComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/moduleSettings.h"
#include "engine/core/events/eventConsumer.h"
#include "engine/core/events/eventRules.h"
#include "engine/core/events/eventTraits.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/reflection/registration.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/navigation/agentEvent.h"
#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/coarseGraph.h"
#include "engine/runtime/navigation/fleet.h"
#include "engine/runtime/navigation/geometry.h"
#include "engine/runtime/navigation/planScheduler.h"
#include "engine/runtime/navigation/vehicle/speedProfile.h"
#include "engine/runtime/navigation/vehicle/vehicleKind.h"
#include "engine/runtime/navigation/vehicle/vehiclePath.h"
#include "engine/runtime/navigation/vehicle/vehicleSpec.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/systems/navigationSystem.h"
#include "engine/runtime/systems/physicsSystem3D.h"

#include <glm/glm/ext/matrix_float4x4.hpp>
#include <glm/glm/ext/matrix_transform.hpp>
#include <glm/glm/ext/quaternion_float.hpp>
#include <glm/glm/ext/quaternion_trigonometric.hpp>
#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <glm/glm/geometric.hpp>
#include <glm/glm/gtc/quaternion.hpp>
#include <glm/glm/trigonometric.hpp>

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sfs;

// The engine's own event types get their traits from its build's scan.
static_assert(detail::kKind<nav::AgentEvent> == detail::EventKind::EntityNews);

namespace
{
namespace fs = std::filesystem;
constexpr double FRAME = 1.0 / 30.0;

// News about an agent as a test keeps it.
struct Heard
{
  Entity agent;
  nav::Fleet::Event::Kind kind = nav::Fleet::Event::Kind::Arrived;
  std::size_t blockers = 0;
};

// A system after the navigation in the update order: what it heard in its
// last update.
class AgentListener : public System
{
public:
  std::vector<Heard> heard;

protected:
  void create() override { m_news = consumer<nav::AgentEvent>(); }

  void update(double) override
  {
    heard.clear();

    for (auto [agent, news] : m_news.read())
      heard.push_back({agent, news.what.kind, news.what.blockers.size()});
  }

private:
  EventConsumer<nav::AgentEvent> m_news;
};

// A game's extension of the spec (a game's TruckSpec, say), registered as
// its generated code would.
struct TestTruckSpec : VehicleSpec
{
  float hitchHeight = 0.5f;
};

// The project's vehicle specs: a car (a VehicleSpec) and a truck (a
// TestTruckSpec), as .asset files the process-wide assets read.
struct Specs
{
  fs::path root = fs::temp_directory_path() / "sfs_nav_specs";
  std::unique_ptr<AssetDatabase> catalogue;
  std::string car, truck;

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
    write("Truck",
          "TestTruckSpec",
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
    truck = refOf("Truck");
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
  Scene scene{0, SceneBackends{.assetStore = store}};
  PhysicsSystem3D& physics = scene.addSystem<PhysicsSystem3D>();
  NavigationSystem& navigation = scene.addSystem<NavigationSystem>();
  // What the navigation's agents heard, read after each frame.
  EventConsumer<nav::AgentEvent> agentNews =
      scene.events().consumer<nav::AgentEvent>();

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

  // One frame, then waits till the planning it asked for is done, so a plan
  // takes the same frames however fast the machine runs it.
  void step()
  {
    scene.update(FRAME);

    while (navigation.scheduler().pending() > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Run till `agent` arrives or is blocked (or `seconds` pass).
  std::vector<Heard> run(const Entity& agent, double seconds)
  {
    std::vector<Heard> events;

    for (double t = 0.0; t < seconds; t += FRAME)
    {
      step();

      for (auto [about, news] : agentNews.read())
        events.push_back({about, news.what.kind, news.what.blockers.size()});

      const nav::Fleet::State st = navigation.state(agent);

      if (st == nav::Fleet::State::Arrived || st == nav::Fleet::State::Blocked)
        break;
    }

    return events;
  }
};

// A straight path a game made for `agent`, along x from x0 to x1 at y,
// profiled by the agent's own spec.
std::shared_ptr<const nav::AgentPath>
straightPath(const nav::AgentSpec& agent, float x0, float x1, float y)
{
  nav::vehicle::VehiclePath p;

  for (float x = x0; x <= x1 + 1.0e-4f; x += 0.1f)
  {
    nav::vehicle::PathPoint q;
    q.s = x - x0;
    q.pose = {{x, y}, 0.0f};
    p.points.push_back(q);
  }

  p.start = p.points.front().pose;
  nav::vehicle::profile(p, nav::vehicle::modelOf(agent));
  return std::make_shared<nav::vehicle::VehicleAgentPath>(std::move(p));
}

void registerTruckSpec()
{
  using namespace reflect;
  TypeInfo type = derivedType<TestTruckSpec>(
      "TestTruckSpec", "Test Truck Spec", "navTests", "VehicleSpec");
  FieldInfo field;
  field.name = "hitchHeight";
  field.kind = FieldKind::Float;
  field.address = [](void* o) -> void*
  { return &objectAs<TestTruckSpec>(o)->hitchHeight; };
  type.fields.push_back(field);
  types().add(type);
}
} // namespace

int main()
{
  registerTruckSpec();

  TEST("VehicleSpec should be reflected as the engine's, and a game's spec "
       "derived from it should have its fields first")
  {
    const reflect::TypeInfo* spec = reflect::types().find("VehicleSpec");
    CHECK(spec && spec->kind == reflect::TypeKind::ScriptableObject &&
          spec->module == "engine");
    const reflect::TypeInfo* truck = reflect::types().find("TestTruckSpec");
    CHECK(truck && spec && truck->fields.size() == spec->fields.size() + 1);
    CHECK(truck && truck->fields.front().name == "wheelbase" &&
          truck->fields.back().name == "hitchHeight");
    CHECK(reflect::types().isA("TestTruckSpec", "VehicleSpec"));
  }

  TEST("a scene should be able to list NavigationSystem by name")
  {
    const ScriptType* type = scripts().find("NavigationSystem");
    CHECK(type != nullptr);
    CHECK(type && type->kind == ScriptType::Kind::System);
  }

  TEST("a game's shared hold on the planning scheduler should be the system's "
       "own scheduler, and a job on it should run there")
  {
    World world;
    std::shared_ptr<nav::PlanScheduler> shared =
        world.navigation.sharedScheduler();
    CHECK(shared.get() == &world.navigation.scheduler());
    const nav::PlanScheduler::JobId job = shared->submitOne(
        7,
        "test",
        nav::PlanScheduler::Clock::now(),
        [](const std::atomic<bool>&) { return std::any(42); });
    nav::PlanScheduler::Collected in;
    std::vector<std::any> legs;

    for (;;)
    {
      in = shared->collect(job);
      legs.insert(legs.end(), in.legs.begin(), in.legs.end());

      if (in.state == nav::PlanScheduler::State::Done)
        break;

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(in.state == nav::PlanScheduler::State::Done);
    CHECK(legs.size() == 1 && std::any_cast<int>(legs.front()) == 42);
  }

  TEST("an agent sent past a wall should drive round it and arrive, its "
       "transform moved by the system")
  {
    Specs specs;
    World world;
    world.solid({12.0f, 0.0f, 1.0f}, {1.0f, 8.0f, 2.0f});
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    world.scene.update(FRAME); // (the physics picks up its bodies)
    world.navigation.requestGoal(car, {{{24.0f, 0.0f}, 0.0f}});
    const auto events = world.run(car, 90.0);
    CHECK(world.navigation.state(car) == nav::Fleet::State::Arrived);
    const glm::vec3 at = hierarchy::worldPosition(car);
    CHECK(glm::length(glm::vec2(at) - glm::vec2(24.0f, 0.0f)) < 0.1f);
    bool arrived = false;

    for (const Heard& e : events)
      arrived = arrived ||
                (e.agent == car && e.kind == nav::Fleet::Event::Kind::Arrived);

    CHECK(arrived);
  }

  TEST("an agent's news should be sent as nav::AgentEvent about its entity, "
       "and a system after the navigation should hear it in the same update")
  {
    Specs specs;
    World world;
    AgentListener& listener = world.scene.addSystem<AgentListener>();
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    world.scene.update(FRAME);
    world.navigation.requestGoal(car, {{{10.0f, 0.0f}, 0.0f}});
    bool heardOnArrival = false;
    int arrivals = 0;

    for (double t = 0.0; t < 60.0; t += FRAME)
    {
      const bool before =
          world.navigation.state(car) == nav::Fleet::State::Arrived;
      world.step();

      for (const Heard& e : listener.heard)
        if (e.agent == car && e.kind == nav::Fleet::Event::Kind::Arrived)
        {
          ++arrivals;
          heardOnArrival = !before && world.navigation.state(car) ==
                                          nav::Fleet::State::Arrived;
        }

      if (arrivals > 0)
        break;
    }

    CHECK(arrivals == 1);
    CHECK(heardOnArrival);
    // Each event is heard once: not again at the next update.
    world.scene.update(FRAME);
    bool again = false;

    for (const Heard& e : listener.heard)
      again = again || e.kind == nav::Fleet::Event::Kind::Arrived;

    CHECK(!again);
  }

  TEST("an agent sent into a solid should be proven blocked, its AgentEvent "
       "about the agent bringing what stands on its goal")
  {
    Specs specs;
    World world;
    world.solid({12.0f, 0.0f, 1.0f}, {6.0f, 6.0f, 2.0f});
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    world.scene.update(FRAME);
    world.navigation.requestGoal(car, {{{12.0f, 0.0f}, 0.0f}});
    const std::vector<Heard> events = world.run(car, 30.0);
    CHECK(world.navigation.state(car) == nav::Fleet::State::Blocked);
    int blocked = 0;
    std::size_t blockers = 0;

    for (const Heard& e : events)
      if (e.agent == car && e.kind == nav::Fleet::Event::Kind::Blocked)
      {
        ++blocked;
        blockers = e.blockers;
      }

    CHECK(blocked == 1);
    CHECK(blockers > 0u);
  }

  TEST("a game's coarse edges (its worn tracks) should be put on the coarse "
       "graph on the scheduler, and on again when they change, the graph "
       "plans held keep as it was")
  {
    Specs specs;
    World world;
    world.solid({12.0f, 0.0f, 1.0f}, {1.0f, 8.0f, 2.0f});
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    // Frames till the graph has `edges` edges, however long the scheduler's
    // workers take to make it.
    // Frames till the graph has `edges` edges: each waits for the scheduler,
    // so it's in within a couple. If it isn't in 600, what the frames saw is
    // printed: how often the physics changed, what the scheduler ran, the
    // graphs that came.
    const auto graphWith = [&](std::size_t edges)
    {
      int revisions = 0, graphs = 0;
      uint64_t revision = world.physics.revision();
      const nav::CoarseGraph* last = nullptr;

      for (int frame = 0; frame < 600; ++frame)
      {
        world.step();
        const auto& g = world.navigation.fleet().world().graph;

        if (g && g->edges().size() == edges)
          return g;

        revisions += world.physics.revision() != revision;
        revision = world.physics.revision();
        graphs += g && g.get() != last;
        last = g.get();
      }

      const nav::PlanScheduler::Stats stats =
          world.navigation.scheduler().stats();
      std::printf("      no graph of %zu edges in 600 frames: the physics "
                  "changed in %d, %d graphs came (the last of %zu edges), the "
                  "scheduler took %llu jobs, ran %llu steps, has %zu pending\n",
                  edges,
                  revisions,
                  graphs,
                  last ? last->edges().size() : 0,
                  static_cast<unsigned long long>(stats.jobs),
                  static_cast<unsigned long long>(stats.steps),
                  world.navigation.scheduler().pending());
      return std::shared_ptr<const nav::CoarseGraph>();
    };
    const auto plain = graphWith(0);
    CHECK(plain != nullptr);
    const nav::CoarseGraph::Edge a{{0.0f, -6.0f}, {20.0f, -6.0f}, 0.85f, 2.0f};
    const nav::CoarseGraph::Edge b{{20.0f, -6.0f}, {24.0f, 0.0f}, 0.85f, 2.0f};
    world.navigation.setCoarseEdges({a, b});
    const auto worn = graphWith(2);
    CHECK(worn != nullptr && worn != plain);
    CHECK(plain && plain->edges().empty());
    world.navigation.setCoarseEdges({b});
    const auto less = graphWith(1);
    CHECK(less != nullptr && less->edges().front() == b);
    CHECK(world.navigation.coarseEdges().size() == 1u);
    (void)car;
  }

  TEST("a player at the wheel should drive the agent from where it was put, "
       "the system writing its transform, and hand it back where it stands")
  {
    Specs specs;
    World world;
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    world.scene.update(FRAME);
    // (Put somewhere by the game since the last update: driven from there.)
    TransformComponent* t = car.getComponent<TransformComponent>();
    t->position = {5.0f, 3.0f, 0.0f};
    CHECK(world.navigation.takeWheel(car));
    CHECK(world.navigation.handDriven(car));
    nav::Controls c;
    c.throttle = 1.0f;
    world.navigation.setControls(car, c);

    for (int i = 0; i < 60; ++i)
      world.scene.update(FRAME);

    const glm::vec3 at = hierarchy::worldPosition(car);
    CHECK(at.x > 5.5f && std::abs(at.y - 3.0f) < 1.0e-3f);
    world.navigation.handBack(car);

    for (int i = 0; i < 30; ++i)
      world.scene.update(FRAME);

    CHECK(!world.navigation.handDriven(car));
    CHECK(glm::length(glm::vec2(hierarchy::worldPosition(car) - at)) < 0.5f);
  }

  TEST("a vehicle agent should drive by a derived spec (a game's "
       "TruckSpec) through its VehicleComponent, and one without a "
       "VehicleComponent should be left out")
  {
    Specs specs;
    World world;
    Entity truck = world.car({0.0f, 0.0f}, 0.0f, specs.truck);
    Entity bare = world.car({0.0f, 10.0f}, 0.0f, {});
    world.scene.update(FRAME);
    AssetRef<VehicleSpec> ref;
    ref.asset = specs.truck;
    CHECK(dynamic_cast<const TestTruckSpec*>(ref.get()) != nullptr);
    CHECK(scriptableObjects().list("VehicleSpec").size() == 2u);
    world.navigation.requestGoal(truck, {{{15.0f, 0.0f}, 0.0f}});
    world.navigation.requestGoal(bare, {{{15.0f, 10.0f}, 0.0f}});
    world.run(truck, 60.0);
    CHECK(world.navigation.state(truck) == nav::Fleet::State::Arrived);
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

  TEST("one AgentSpec: the agent's spec should be applySpec of its "
       "VehicleSpec and the rig the game's provider gives (the game's planners "
       "call the same), a measured wheelbase used only where the spec's is 0")
  {
    Specs specs;
    World world;
    Entity truck = world.car({0.0f, 0.0f}, 0.0f, specs.truck);
    nav::vehicle::Rig given;
    given.extraMass = 1234.5f;
    given.grip = 0.437f;
    given.wheelbase = 1.1f;
    given.effort = 0.9f; // (not the provider's: the component's)
    world.navigation.setRigProvider([&](const Entity&) { return given; });
    world.scene.update(FRAME);
    const nav::AgentSpec* driven =
        world.navigation.fleet().spec(static_cast<uint64_t>(truck.getId()));
    CHECK(driven != nullptr);
    AssetRef<VehicleSpec> ref;
    ref.asset = specs.truck;
    nav::vehicle::Rig rig = given;
    rig.effort = NavAgentComponent{}.effort;
    nav::AgentSpec planned;
    nav::vehicle::applySpec(planned, *ref.get(), rig);

    if (driven)
    {
      CHECK(driven->accel == planned.accel && driven->brake == planned.brake);
      CHECK(driven->lateralAccel == planned.lateralAccel);
      CHECK(driven->minRadius == planned.minRadius);
      CHECK(driven->wheelbase == ref.get()->wheelbase); // (the spec's wins)
    }

    // (A load changed: re-derived, unrounded.)
    given.extraMass = 1300.25f;
    world.scene.update(FRAME);
    rig.extraMass = given.extraMass;
    nav::vehicle::applySpec(planned, *ref.get(), rig);
    CHECK(world.navigation.fleet()
              .spec(static_cast<uint64_t>(truck.getId()))
              ->accel == planned.accel);
    // (A spec whose wheelbase is 0, "measure it": the measured one.)
    VehicleSpec measure;
    measure.wheelbase = 0.0f;
    nav::AgentSpec measured;
    nav::vehicle::applySpec(measured, measure, given);
    CHECK(measured.wheelbase == 1.1f);
    CHECK(std::abs(measured.minRadius -
                   1.1f / std::tan(glm::radians(measure.maxSteerAngle))) <
          1.0e-4f);
  }

  TEST("a measured fixed axle (the rig's, where the spec's is 0) is the "
       "agent's reference point: its pose is the axle's, the entity's origin "
       "stays where it is")
  {
    Specs specs;
    World world;
    Entity truck = world.car({4.0f, 2.0f}, 0.5f * nav::kPi, specs.truck);
    nav::vehicle::Rig given;
    given.axle = -1.5f; // (behind its origin)
    world.navigation.setRigProvider([&](const Entity&) { return given; });
    world.scene.update(FRAME);
    const nav::Pose2 pose =
        world.navigation.fleet().pose(static_cast<uint64_t>(truck.getId()));
    CHECK(glm::length(pose.position - glm::vec2(4.0f, 0.5f)) < 1.0e-3f);
    CHECK(std::abs(pose.heading - 0.5f * nav::kPi) < 1.0e-3f);
    world.scene.update(FRAME);
    CHECK(glm::length(glm::vec2(hierarchy::worldTransform(truck).position) -
                      glm::vec2(4.0f, 2.0f)) < 1.0e-3f);
  }

  TEST("its debug toggles should be offered as settings")
  {
    World world;
    bool paths = false;

    for (const ModuleSetting& s : world.navigation.settings())
      paths = paths || s.label == "Draw paths";

    CHECK(paths);
  }

  TEST("a mounted tool (rigid hitch) should join the agent's boxes; one "
       "left out (active false) should leave the fleet till it's back")
  {
    Specs specs;
    World world;
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    Entity tool = world.solid({-2.0f, 0.0f, 0.5f}, {1.0f, 3.0f, 1.0f});
    world.scene.update(FRAME);
    const auto id = static_cast<nav::Fleet::AgentId>(car.getId());
    const std::size_t alone = world.navigation.fleet().spec(id)->boxes.size();
    NavHitch mounted;
    mounted.towed = tool;
    mounted.rigid = true;
    car.getComponent<NavAgentComponent>()->hitch = mounted;
    world.scene.update(FRAME);
    const nav::AgentSpec* spec = world.navigation.fleet().spec(id);
    CHECK(spec->boxes.size() == alone + 1);
    CHECK(!spec->trailer);
    car.getComponent<NavAgentComponent>()->active = false;
    world.scene.update(FRAME);
    CHECK(!world.navigation.fleet().has(id));
    car.getComponent<NavAgentComponent>()->active = true;
    world.scene.update(FRAME);
    CHECK(world.navigation.fleet().has(id));
  }

  TEST("a destroyed agent's hitch, speed cap and leave-out should go with it: "
       "a new agent on its reused id starts clean, in the fleet")
  {
    Specs specs;
    World world;
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    Entity tool = world.solid({-2.0f, 0.0f, 0.5f}, {1.0f, 3.0f, 1.0f});
    world.scene.update(FRAME);
    const auto id = static_cast<nav::Fleet::AgentId>(car.getId());
    const std::size_t alone = world.navigation.fleet().spec(id)->boxes.size();
    NavHitch mounted;
    mounted.towed = tool;
    mounted.rigid = true;
    car.getComponent<NavAgentComponent>()->hitch = mounted;
    car.getComponent<NavAgentComponent>()->speedCap = 0.0f;
    car.getComponent<NavAgentComponent>()->active = false;
    world.scene.update(FRAME);
    CHECK(!world.navigation.fleet().has(id));
    world.scene.destroyEntity(car);
    world.scene.flushEntities();
    Entity next = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    CHECK(static_cast<nav::Fleet::AgentId>(next.getId()) == id);
    world.scene.update(FRAME);
    CHECK(world.navigation.fleet().has(id));
    CHECK(world.navigation.fleet().spec(id)->boxes.size() == alone);
    const NavAgentComponent* fresh =
        next.getComponent<const NavAgentComponent>();
    CHECK(!fresh->hitch && fresh->active && std::isinf(fresh->speedCap));
  }

  TEST("a mounted part's box should be its true size in the agent's own "
       "frame (mountedBox): as its mount holds it (NavHitch::mount), never "
       "with the body's pitch baked in; exactly where it's held")
  {
    Specs specs;
    World world;
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    // (A 3 m x 4 m x 1 m blade ahead of the car, pitched 14 degrees up by
    // its lift, the car's body pitched 5 degrees nose down on the ground.)
    Entity tool = world.solid({5.0f, 0.0f, 1.5f}, {3.0f, 4.0f, 1.0f});
    const glm::quat lifted =
        glm::angleAxis(glm::radians(-14.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat nosedDown =
        glm::angleAxis(glm::radians(5.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    car.getComponent<TransformComponent>()->rotation = nosedDown;
    tool.getComponent<TransformComponent>()->rotation = nosedDown * lifted;
    tool.getComponent<TransformComponent>()->position = glm::vec3(
        glm::mat4_cast(nosedDown) * glm::vec4(5.0f, 0.0f, 1.5f, 1.0f));
    NavHitch mounted;
    mounted.towed = tool;
    mounted.rigid = true;
    // (Its authored mount, lowered: level, 5 m ahead, 1 m up.)
    mounted.mount =
        glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 1.0f));
    car.getComponent<NavAgentComponent>()->hitch = mounted;
    world.scene.update(FRAME);
    const auto id = static_cast<nav::Fleet::AgentId>(car.getId());
    const nav::AgentSpec* spec = world.navigation.fleet().spec(id);
    CHECK(spec && spec->boxes.size() == 2u);

    if (spec && spec->boxes.size() == 2u)
    {
      // (Its true size from the mount, wherever it's held and however the
      // body's pitched as it's measured.)
      const nav::Box3 b = spec->boxes.back();
      CHECK(std::abs(b.back - 3.5f) < 1.0e-4f);
      CHECK(std::abs(b.front - 6.5f) < 1.0e-4f);
      CHECK(std::abs(b.half - 2.0f) < 1.0e-4f);
      CHECK(std::abs(b.bottom - 0.5f) < 1.0e-4f);
      CHECK(std::abs(b.top - 1.5f) < 1.0e-4f);
      const nav::Box3 same = nav::mountedBox(*mounted.mount,
                                             glm::vec3(-1.5f, -2.0f, -0.5f),
                                             glm::vec3(1.5f, 2.0f, 0.5f));
      CHECK(b.back == same.back && b.front == same.front);
      CHECK(b.bottom == same.bottom && b.top == same.top);
    }

    // (No mount given: where it's held now relative to the agent, the
    // lift's 14 degrees but not the body's 5: in the agent's own frame.)
    mounted.mount.reset();
    car.getComponent<NavAgentComponent>()->hitch = mounted;
    world.scene.update(FRAME);
    spec = world.navigation.fleet().spec(id);
    CHECK(spec && spec->boxes.size() == 2u);

    if (spec && spec->boxes.size() == 2u)
    {
      const nav::Box3 b = spec->boxes.back();
      const float c = std::cos(glm::radians(14.0f)),
                  sn = std::sin(glm::radians(14.0f));
      CHECK(std::abs(b.front - (5.0f + 1.5f * c + 0.5f * sn)) < 1.0e-3f);
      CHECK(std::abs(b.back - (5.0f - 1.5f * c - 0.5f * sn)) < 1.0e-3f);
      CHECK(std::abs(b.top - (1.5f + 1.5f * sn + 0.5f * c)) < 1.0e-3f);
    }
  }

  TEST("a towed trailer: its box measured from its axle (NavHitch::axle off "
       "its origin), its drawbar to that axle, and the angle it was left at "
       "taken while nothing moves the agent")
  {
    Specs specs;
    World world;
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    // (A 4 m x 2 m trailer, its axle 0.5 m behind its middle, swung 0.2 rad.)
    Entity trailer = world.solid({-6.0f, 0.0f, 0.5f}, {4.0f, 2.0f, 1.0f});
    trailer.getComponent<TransformComponent>()->rotation =
        glm::angleAxis(0.2f, glm::vec3(0.0f, 0.0f, 1.0f));
    NavHitch towed;
    towed.towed = trailer;
    towed.hitchBehind = 1.0f;
    towed.drawbar = 4.0f;
    towed.axle = -0.5f;
    car.getComponent<NavAgentComponent>()->hitch = towed;
    world.scene.update(FRAME);
    const auto id = static_cast<nav::Fleet::AgentId>(car.getId());
    const nav::AgentSpec* spec = world.navigation.fleet().spec(id);
    CHECK(spec && spec->trailer.has_value());

    if (spec && spec->trailer)
    {
      CHECK(std::abs(spec->trailer->drawbar - 4.0f) < 1.0e-4f);
      CHECK(std::abs(spec->trailer->box.back - -1.5f) < 1.0e-3f);
      CHECK(std::abs(spec->trailer->box.front - 2.5f) < 1.0e-3f);
    }

    CHECK(std::abs(world.navigation.fleet().hitch(id) - -0.2f) < 1.0e-3f);
  }

  TEST("a speed cap written on NavAgentComponent should hold the agent, and "
       "reach the fleet at once for every order that plans, as at its update")
  {
    Specs specs;
    World world;
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    world.scene.update(FRAME);
    const auto id = static_cast<nav::Fleet::AgentId>(car.getId());
    NavAgentComponent* agent = car.getComponent<NavAgentComponent>();
    const nav::AgentSpec* spec = world.navigation.fleet().spec(id);
    CHECK(spec != nullptr);
    const nav::AgentSpec planned = spec ? *spec : nav::AgentSpec{};

    agent->speedCap = 0.0f;
    world.navigation.setPath(car, straightPath(planned, 0.0f, 30.0f, 0.0f));
    CHECK(world.navigation.fleet().speedCap(id) == 0.0f);

    for (int frame = 0; frame < 90; ++frame)
      world.step();

    CHECK(world.navigation.fleet().along(id) < 0.2f);
    agent->speedCap = 2.0f;
    world.navigation.planAhead(car, {});
    CHECK(world.navigation.fleet().speedCap(id) == 2.0f);
    agent->speedCap = 1.5f;
    (void)world.navigation.planNow(car, {});
    CHECK(world.navigation.fleet().speedCap(id) == 1.5f);
    agent->speedCap = 1.0f;
    world.navigation.requestGoal(car, {{{30.0f, 0.0f}, 0.0f}});
    CHECK(world.navigation.fleet().speedCap(id) == 1.0f);
    agent->speedCap = 0.5f;
    world.step();
    CHECK(world.navigation.fleet().speedCap(id) == 0.5f);
    agent->speedCap = std::numeric_limits<float>::infinity();
    float fastest = 0.0f;

    for (int frame = 0; frame < 150; ++frame)
    {
      world.step();
      fastest = std::max(fastest, world.navigation.fleet().speed(id));
    }

    CHECK(fastest > 2.0f);
  }

  TEST("an agent's news should name the other agent it's about by its "
       "entity (AgentEvent::other)")
  {
    Specs specs;
    World world;
    Entity car = world.car({0.0f, 0.0f}, 0.0f, specs.car);
    Entity parked = world.car({20.0f, 0.0f}, 0.0f, specs.car);
    world.scene.update(FRAME);
    const auto id = static_cast<nav::Fleet::AgentId>(car.getId());
    const nav::AgentSpec* spec = world.navigation.fleet().spec(id);
    CHECK(spec != nullptr);
    const nav::AgentSpec planned = spec ? *spec : nav::AgentSpec{};

    world.navigation.setPath(car, straightPath(planned, 0.0f, 40.0f, 0.0f));
    bool named = false;
    bool none = true;

    for (int frame = 0; frame < 30 * 10 && !named; ++frame)
    {
      world.step();

      for (auto [about, news] : world.agentNews.read())
      {
        if (news.what.kind == nav::Fleet::Event::Kind::NeedsPlan &&
            about == car)
          named = news.other == parked;

        none = none && (news.what.other != 0 || !news.other.isValid());
      }
    }

    CHECK(named);
    CHECK(none);
  }

  return testing::report("navigationSystemTests");
}
