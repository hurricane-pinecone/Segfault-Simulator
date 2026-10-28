#include "../../testHarness.h"

#include <engine/core/components/scriptComponent.h>
#include <engine/core/components/transform3DComponent.h>
#include <engine/core/reflection/reflection.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/game/gameObject.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/runtime/scripting/scriptRegistry.h>

#include <vector>

using namespace sfs;

namespace
{
struct Counter
{
  int ticks = 0;
};

// A game system as a module would declare it.
class TickSystem : public System
{
protected:
  void create() override { registerComponent<Counter>(); }
  void update(double) override
  {
    for (Entity& entity : getEntities())
      ++entity.getComponent<Counter>().ticks;
  }
};

// A game object script that overrides its hooks as protected, like the
// editor's template.
class Mover : public GameObject
{
public:
  static inline int created = 0;

protected:
  void onCreate(Scene&) override
  {
    ++created;
    boundToExisting = entity().hasComponent<Transform3DComponent>();
    speedsAtCreate.push_back(speed);
  }
  void onUpdate(double deltaTime) override
  {
    entity().getComponent<Transform3DComponent>().position.x +=
        speed * static_cast<float>(deltaTime);
  }

public:
  float speed = 1.0f; // reflected below, as codegen would
  bool boundToExisting = false;
  static inline std::vector<float> speedsAtCreate;
};

void registerTestScripts()
{
  ScriptType tick;
  tick.name = "TickSystem";
  tick.module = "test";
  tick.kind = ScriptType::Kind::System;
  tick.addSystem = [](Scene& scene) -> System&
  { return scene.addSystem<TickSystem>(); };
  scripts().add(tick);

  ScriptType mover;
  mover.name = "Mover";
  mover.module = "test";
  mover.kind = ScriptType::Kind::GameObject;
  mover.createObject = []() -> std::unique_ptr<GameObject>
  { return std::make_unique<Mover>(); };
  scripts().add(mover);

  reflect::TypeInfo moverType;
  moverType.name = "Mover";
  moverType.module = "test";
  moverType.kind = reflect::TypeKind::GameObject;
  reflect::FieldInfo speed;
  speed.name = "speed";
  speed.kind = reflect::FieldKind::Float;
  speed.address = [](void* object) -> void*
  { return &static_cast<Mover*>(object)->speed; };
  moverType.fields.push_back(speed);
  reflect::types().add(moverType);
}

struct World
{
  AssetStore store;
  Scene scene{0, SceneServices{.assetStore = store}};
};
} // namespace

int main()
{
  registerTestScripts();

  TEST("startScripts should attach game objects to their entities")
  {
    World world;
    Entity box = world.scene.createEntity();
    box.addComponent<Transform3DComponent>();
    box.addComponent<ScriptComponent>("Mover");
    Entity plain = world.scene.createEntity();
    plain.addComponent<Transform3DComponent>();
    world.scene.flushEntities();

    Mover::created = 0;
    startScripts(world.scene, {});
    CHECK(Mover::created == 1);

    Mover* mover = world.scene.tryFindObject<Mover>();
    CHECK(mover != nullptr);
    CHECK(mover->boundToExisting);
    CHECK(mover->entity().getId() == box.getId());

    world.scene.update(0.5);
    CHECK(testing::approx(
        box.getComponent<Transform3DComponent>().position.x, 0.5f));
    CHECK(testing::approx(
        plain.getComponent<Transform3DComponent>().position.x, 0.0f));
  }

  TEST("an entity's saved script fields should be set before onCreate")
  {
    World world;
    Entity fast = world.scene.createEntity();
    fast.addComponent<Transform3DComponent>();
    ScriptComponent script("Mover");
    script.fields = {{"speed", 4.0}, {"removedField", 1}};
    fast.addComponent<ScriptComponent>(script);
    Entity standard = world.scene.createEntity();
    standard.addComponent<Transform3DComponent>();
    standard.addComponent<ScriptComponent>(
        "Mover"); // no saved values: defaults
    world.scene.flushEntities();

    Mover::speedsAtCreate.clear();
    startScripts(world.scene, {});
    CHECK(Mover::speedsAtCreate.size() == 2u);
    CHECK(testing::approx(Mover::speedsAtCreate[0], 4.0f)); // onCreate saw it

    world.scene.update(0.5);
    CHECK(testing::approx(
        fast.getComponent<Transform3DComponent>().position.x, 2.0f));
    CHECK(testing::approx(
        standard.getComponent<Transform3DComponent>().position.x, 0.5f));
  }

  TEST("startScripts should add game systems that see existing entities")
  {
    World world;
    Entity counted = world.scene.createEntity();
    counted.addComponent<Counter>();
    world.scene.flushEntities();

    startScripts(world.scene, {"TickSystem"});
    world.scene.update(0.1);
    world.scene.update(0.1);
    CHECK(counted.getComponent<Counter>().ticks == 2);
  }

  TEST("unknown scripts should be skipped, not crash")
  {
    World world;
    Entity entity = world.scene.createEntity();
    entity.addComponent<ScriptComponent>("Missing");
    world.scene.flushEntities();

    startScripts(world.scene, {"NoSuchSystem"});
    CHECK(world.scene.tryFindObject<Mover>() == nullptr);
  }

  TEST("a module's scripts should be removable as a unit")
  {
    ScriptRegistry registry;
    ScriptType a;
    a.name = "A";
    a.module = "game";
    ScriptType b;
    b.name = "B";
    b.module = "other";
    registry.add(a);
    registry.add(b);
    registry.removeModule("game");
    CHECK(registry.find("A") == nullptr);
    CHECK(registry.find("B") != nullptr);
  }

  return testing::report("scriptRegistryTests");
}
