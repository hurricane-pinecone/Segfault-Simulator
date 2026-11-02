#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>
#include <engine/core/util/fatalError.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace sfs;

namespace
{
struct Position
{
  float x = 0.0f;
  float y = 0.0f;
};
struct Velocity
{
  float dx = 0.0f;
  float dy = 0.0f;
};
struct Health
{
  int hp = 100;
};
struct Enemy
{
}; // data-free tag

// Runs `body`, which should stop with a FatalError; its message, or empty if
// it didn't.
template <typename Body>
std::string fatalMessage(Body&& body)
{
  try
  {
    body();
  }
  catch (const FatalError& error)
  {
    return error.what();
  }
  return {};
}

template <int N>
struct Numbered
{
  int value = N;
};

// Every Numbered<0>..Numbered<N-1> registers a component type.
template <int... Ns>
void registerNumbered(std::integer_sequence<int, Ns...>)
{
  (Component<Numbered<Ns>>::getId(), ...);
}

// Interested in entities that have both Position and Velocity.
class MovementSystem : public System
{
public:
  int created = 0;

protected:
  void create() override
  {
    registerComponent<Position>();
    registerComponent<Velocity>();
    ++created;
  }
};
} // namespace

int main()
{
  TEST("a default-constructed entity should be invalid")
  {
    Entity none;
    CHECK(!none.isValid());
    CHECK(!static_cast<bool>(none));
    CHECK(none.getId() == Entity::InvalidId);
  }

  TEST("createEntity should return unique, live entities")
  {
    Registry reg;
    Entity a = reg.createEntity();
    Entity b = reg.createEntity();
    CHECK(a.isValid());
    CHECK(a.isAlive());
    CHECK(b.isAlive());
    CHECK(a.getId() != b.getId());
    CHECK(a.getDebugId() != b.getDebugId());
    CHECK(a != b);
  }

  TEST("a pool should store, fetch, and remove elements")
  {
    Pool<int> pool;
    CHECK(pool.isEmpty());
    pool.set(2, 42); // grows to hold index 2
    CHECK(pool.getSize() == 3);
    CHECK(pool.has(2));
    CHECK(!pool.has(0));
    CHECK(pool.get(2) == 42);
    CHECK(pool[2] == 42);

    // remove(size_t) clears by index; the cast is needed because a bare int
    // binds to remove(const T&) (the by-value overload) instead.
    pool.remove(static_cast<std::size_t>(2));
    CHECK(!pool.has(2));

    pool.set(0, 7);
    CHECK(pool.has(0));
    pool.remove(7); // remove(const T&) clears the slot holding that value
    CHECK(!pool.has(0));

    pool.clear();
    CHECK(pool.isEmpty());
  }

  TEST("an entity should add, read, mutate, and remove components")
  {
    Registry reg;
    Entity e = reg.createEntity();
    CHECK(!e.hasComponent<Position>());

    e.addComponent<Position>(Position{1.0f, 2.0f});
    e.addComponent<Velocity>(Velocity{3.0f, 4.0f});
    CHECK(e.hasComponent<Position>());
    CHECK(e.hasComponent<Velocity>());
    CHECK(!e.hasComponent<Health>());

    CHECK(testing::approx(e.getComponent<Position>().x, 1.0f));
    CHECK(testing::approx(e.getComponent<Velocity>().dy, 4.0f));

    e.getComponent<Position>().x =
        9.0f; // mutation through the reference persists
    CHECK(testing::approx(e.getComponent<Position>().x, 9.0f));

    e.removeComponent<Position>();
    CHECK(!e.hasComponent<Position>());
    CHECK(e.hasComponent<Velocity>());
  }

  TEST("components should be independent per entity")
  {
    Registry reg;
    Entity a = reg.createEntity();
    Entity b = reg.createEntity();
    a.addComponent<Health>(Health{30});
    b.addComponent<Health>(Health{70});
    CHECK(a.getComponent<Health>().hp == 30);
    CHECK(b.getComponent<Health>().hp == 70);
  }

  TEST("addTag should attach a marker component")
  {
    Registry reg;
    Entity e = reg.createEntity();
    e.addTag<Enemy>();
    CHECK(e.hasComponent<Enemy>());
  }

  TEST("view should match entities having all requested components")
  {
    Registry reg;
    Entity moving = reg.createEntity();
    moving.addComponent<Position>();
    moving.addComponent<Velocity>();

    Entity still = reg.createEntity();
    still.addComponent<Position>();

    Entity hurt = reg.createEntity();
    hurt.addComponent<Health>();

    CHECK(reg.view<Position>().size() == 2);           // moving + still
    CHECK(reg.view<Position, Velocity>().size() == 1); // moving only
    CHECK(reg.view<Health>().size() == 1);
    CHECK(reg.view<Velocity, Health>().empty());
  }

  TEST("destroying an entity should bump the generation and reuse its id")
  {
    Registry reg;
    Entity a = reg.createEntity();
    const Entity::EntityId aid = a.getId();
    const uint32_t gen = a.getGeneration();

    reg.destroyEntity(a);
    CHECK(a.isAlive()); // deferred: alive until flush
    reg.flushEntities();
    CHECK(!a.isAlive());
    CHECK(!reg.isAlive(a));

    Entity b = reg.createEntity();
    CHECK(b.getId() == aid);             // id reused
    CHECK(b.getGeneration() == gen + 1); // with a bumped generation
    CHECK(!reg.isAlive(a));              // stale handle stays dead
    CHECK(reg.isAlive(b));
  }

  TEST("view should drop a destroyed entity after flush")
  {
    Registry reg;
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    CHECK(reg.view<Position>().size() == 1);
    reg.destroyEntity(e);
    reg.flushEntities();
    CHECK(reg.view<Position>().empty());
  }

  TEST("systems should register, look up, and remove")
  {
    Registry reg;
    CHECK(!reg.hasSystem<MovementSystem>());
    CHECK(reg.tryGetSystem<MovementSystem>() == nullptr);

    MovementSystem& sys = reg.addSystem<MovementSystem>();
    CHECK(sys.created == 1); // create() ran once
    CHECK(reg.hasSystem<MovementSystem>());
    CHECK(reg.tryGetSystem<MovementSystem>() == &sys);
    CHECK(&reg.getSystem<MovementSystem>() == &sys);

    reg.removeSystem<MovementSystem>();
    CHECK(!reg.hasSystem<MovementSystem>());
    CHECK(reg.tryGetSystem<MovementSystem>() == nullptr);
  }

  TEST("tryGetSystem should return null when the system is absent")
  {
    // getSystem asserts presence and aborts on a missing system, so the
    // recoverable lookup is tryGetSystem, which reports absence as a null.
    Registry reg;
    CHECK(reg.tryGetSystem<MovementSystem>() == nullptr);
  }

  TEST("a system should gain and lose entities on flush")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();

    Entity moving = reg.createEntity();
    moving.addComponent<Position>();
    moving.addComponent<Velocity>();

    Entity still = reg.createEntity();
    still.addComponent<Position>(); // missing Velocity -> not matched

    reg.flushEntities();
    CHECK(sys.getEntities().size() == 1);
    CHECK(sys.getEntities()[0] == moving);

    reg.destroyEntity(moving);
    reg.flushEntities();
    CHECK(sys.getEntities().empty());
  }

  TEST("setEnabled should toggle a system on and off")
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    CHECK(sys.enabled());
    sys.setEnabled(false);
    CHECK(!sys.enabled());
  }

  TEST("a system added after entities exist should pick up the matching ones")
  {
    Registry reg;
    Entity mover = reg.createEntity();
    mover.addComponent<Position>();
    mover.addComponent<Velocity>();
    Entity statue = reg.createEntity();
    statue.addComponent<Position>();
    reg.flushEntities();

    MovementSystem& movement = reg.addSystem<MovementSystem>();
    CHECK(movement.getEntities().size() == 1u);
    CHECK(movement.getEntities().front().getId() == mover.getId());
  }

  TEST("an entity still pending a flush should join a new system only once")
  {
    Registry reg;
    Entity pending = reg.createEntity();
    pending.addComponent<Position>();
    pending.addComponent<Velocity>();

    MovementSystem& movement = reg.addSystem<MovementSystem>();
    CHECK(movement.getEntities().empty());
    reg.flushEntities();
    CHECK(movement.getEntities().size() == 1u);
  }

  TEST(
      "freed ids should be reused lowest-first, whatever order they were freed")
  {
    Registry reg;
    Entity a = reg.createEntity();
    Entity b = reg.createEntity();
    Entity c = reg.createEntity();
    reg.flushEntities();

    reg.destroyEntity(b); // freed first...
    reg.flushEntities();
    reg.destroyEntity(c);
    reg.destroyEntity(a); // ...then the rest
    reg.flushEntities();

    CHECK(reg.createEntity().getId() == a.getId());
    CHECK(reg.createEntity().getId() == b.getId());
    CHECK(reg.createEntity().getId() == c.getId());
  }

  TEST("a component reference should stay good while the pool grows")
  {
    Registry reg;
    Entity first = reg.createEntity();
    first.addComponent<Health>(Health{7});
    Health& health = first.getComponent<Health>();
    for (int i = 0; i < 1000; ++i)
      reg.createEntity().addComponent<Health>(Health{i});
    CHECK(&health == &first.getComponent<Health>());
    CHECK(health.hp == 7);
  }

  TEST("a stale handle should not read the components of its id's new entity")
  {
    Registry reg;
    Entity old = reg.createEntity();
    old.addComponent<Health>(Health{1});
    reg.flushEntities();
    reg.destroyEntity(old);
    reg.flushEntities();

    Entity reused = reg.createEntity();
    reused.addComponent<Health>(Health{2});
    CHECK(reused.getId() == old.getId());
    CHECK(!old.isAlive());
    CHECK(!old.hasComponent<Health>());
    const std::string message =
        fatalMessage([&] { (void)old.getComponent<Health>(); });
    CHECK(message.find("destroyed") != std::string::npos);
    CHECK(message.find("Health") != std::string::npos);
    CHECK(reused.getComponent<Health>().hp == 2);
  }

  TEST("component access on a dead or invalid entity should be a fatal error")
  {
    Registry reg;
    Entity e = reg.createEntity();
    reg.flushEntities();
    CHECK(fatalMessage([&] { (void)e.getComponent<Health>(); })
              .find("has no such component") != std::string::npos);
    reg.destroyEntity(e);
    reg.flushEntities();
    CHECK(!fatalMessage([&] { e.addComponent<Health>(); }).empty());
    e.removeComponent<Health>(); // nothing to remove: no error

    Entity none;
    CHECK(!none.hasComponent<Health>());
    CHECK(!none.isAlive());
    CHECK(!fatalMessage([&] { (void)none.getComponent<Health>(); }).empty());
    CHECK(!fatalMessage([&] { none.addComponent<Health>(); }).empty());
  }

  TEST("removeComponent should destroy the component")
  {
    static int alive = 0;
    struct Counted
    {
      Counted() { ++alive; }
      Counted(const Counted&) { ++alive; }
      ~Counted() { --alive; }
    };
    Registry reg;
    Entity e = reg.createEntity();
    e.addComponent<Counted>();
    CHECK(alive == 1);
    e.removeComponent<Counted>();
    CHECK(alive == 0);
    CHECK(!e.hasComponent<Counted>());
  }

  TEST("adding and removing components should move a live entity between "
       "systems")
  {
    Registry reg;
    MovementSystem& movement = reg.addSystem<MovementSystem>();
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    reg.flushEntities();
    CHECK(movement.getEntities().empty());

    e.addComponent<Velocity>(); // now it matches
    CHECK(movement.getEntities().size() == 1u);
    e.addComponent<Velocity>(Velocity{1.0f, 0.0f}); // again: still once
    CHECK(movement.getEntities().size() == 1u);

    e.removeComponent<Position>(); // no longer
    CHECK(movement.getEntities().empty());
  }

  TEST("destroying many entities should leave each system's list right")
  {
    Registry reg;
    MovementSystem& movement = reg.addSystem<MovementSystem>();
    std::vector<Entity> made;
    for (int i = 0; i < 200; ++i)
    {
      Entity e = reg.createEntity();
      e.addComponent<Position>();
      e.addComponent<Velocity>();
      made.push_back(e);
    }
    reg.flushEntities();
    CHECK(movement.getEntities().size() == 200u);
    for (std::size_t i = 0; i < made.size(); i += 2)
      reg.destroyEntity(made[i]);
    reg.flushEntities();
    CHECK(movement.getEntities().size() == 100u);
    bool onlyOdd = true;
    for (const Entity& e : movement.getEntities())
      onlyOdd = onlyOdd && e.getId() % 2 == 1;
    CHECK(onlyOdd);
  }

  TEST("an entity made and destroyed before a flush should join no system")
  {
    Registry reg;
    MovementSystem& movement = reg.addSystem<MovementSystem>();
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    e.addComponent<Velocity>();
    reg.destroyEntity(e);
    reg.flushEntities();
    CHECK(movement.getEntities().empty());
  }

  TEST("each should visit the matching living entities with their components")
  {
    Registry reg;
    Entity a = reg.createEntity();
    a.addComponent<Position>(Position{1.0f, 0.0f});
    a.addComponent<Velocity>();
    Entity b = reg.createEntity();
    b.addComponent<Position>(Position{2.0f, 0.0f});
    Entity c = reg.createEntity();
    c.addComponent<Position>(Position{3.0f, 0.0f});
    c.addComponent<Velocity>();
    reg.flushEntities();
    reg.destroyEntity(c);
    reg.flushEntities();

    std::vector<float> seen;
    reg.each<Position, Velocity>(
        [&](const Entity& entity, Position& position, Velocity&)
        {
          CHECK(entity == a);
          seen.push_back(position.x);
        });
    CHECK(seen.size() == 1u);
    reg.each<Health>([&](const Entity&, Health&) { seen.clear(); });
    CHECK(seen.size() == 1u); // no entity has had a Health
  }

  TEST("getSystem on a missing system should be a fatal error naming it")
  {
    Registry reg;
    const std::string message =
        fatalMessage([&] { (void)reg.getSystem<MovementSystem>(); });
    CHECK(message.find("MovementSystem") != std::string::npos);
  }

  TEST("more component types than MAX_COMPONENTS should be a fatal error")
  {
    // However many the program has registered so far, this many more is past
    // the limit.
    const std::string message = fatalMessage(
        [] {
          registerNumbered(std::make_integer_sequence<int, MAX_COMPONENTS>());
        });
    CHECK(message.find("MAX_COMPONENTS") != std::string::npos);
    CHECK(message.find("Numbered") != std::string::npos);
  }

  return testing::report("ecsTests");
}
