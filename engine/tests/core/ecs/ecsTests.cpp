#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>
#include <engine/core/util/debugFeatures.h>
#include <engine/core/util/fatalError.h>

#include <algorithm>
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
struct Hidden
{
};

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

// A system type per N, for registries with many kinds of system.
template <int N>
class NumberedSystem : public System
{
};

// Adds a NumberedSystem<N> for each N to `reg`, in order, noting each.
template <int... Ns>
void addNumberedSystems(Registry& reg,
                        std::vector<System*>& added,
                        std::integer_sequence<int, Ns...>)
{
  (added.push_back(&reg.addSystem<NumberedSystem<Ns>>()), ...);
}

// Whether `reg` finds each NumberedSystem<N> as the one added for it.
template <int... Ns>
bool findsNumberedSystems(const Registry& reg,
                          const std::vector<System*>& added,
                          std::integer_sequence<int, Ns...>)
{
  return ((reg.tryGetSystem<NumberedSystem<Ns>>() == added[Ns]) && ...);
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

// Works on movers and observes everything with Health, recording each hook
// and callback as "added"/"removed" lists of (entity id, which: 0 the movers,
// 1 the Health observer), in the order they ran.
class WatchingSystem : public System
{
public:
  std::vector<std::pair<Entity::EntityId, int>> added, removed;
  // Whether a leaving entity still had Position as it left.
  std::vector<bool> leftWithPosition;
  // Every hook and callback, across systems, in the order they ran.
  std::vector<std::string>* log = nullptr;
  std::string name = "watching";

protected:
  void create() override
  {
    registerComponent<Position>();
    registerComponent<Velocity>();
    observe<Health>([this](const Entity& entity) { joined(entity, 1); },
                    [this](const Entity& entity) { left(entity, 1); });
  }
  void onEntityAdded(const Entity& entity) override { joined(entity, 0); }
  void onEntityRemoved(const Entity& entity) override { left(entity, 0); }

private:
  void joined(const Entity& entity, int which)
  {
    added.emplace_back(entity.getId(), which);
    if (log)
      log->push_back(name + "+" + std::to_string(which));
  }
  void left(const Entity& entity, int which)
  {
    removed.emplace_back(entity.getId(), which);
    leftWithPosition.push_back(entity.hasComponent<Position>());
    if (log)
      log->push_back(name + "-" + std::to_string(which));
  }
};

// Whether `list` holds `entity`.
bool lists(const std::vector<Entity>& list, const Entity& entity)
{
  return std::find(list.begin(), list.end(), entity) != list.end();
}

// Observes Health only, with no leave callback, and checks, as each joins,
// that every list it should be in (its Health, and the movers when it moves)
// already holds it.
class HealthCounter : public System
{
public:
  int joined = 0;
  bool listedWhenTold = true;
  std::vector<std::string>* log = nullptr;

protected:
  void create() override
  {
    observe<Health>(
        [this](const Entity& entity)
        {
          ++joined;
          listedWhenTold =
              listedWhenTold && lists(registry->query<Health>(), entity) &&
              (!entity.hasComponent<Velocity>() ||
               lists(registry->query<Position, Velocity>(), entity));
          if (log)
            log->push_back("counter+");
        });
  }
};

// Works on visible positioned entities (Position, Excluding Hidden) and
// observes those with Velocity or Health, recording each hook and callback as
// (entity id, which: 0 its entities, 1 the observer), and whether a leaving
// entity had Hidden as it left.
class VisibleSystem : public System
{
public:
  std::vector<std::pair<Entity::EntityId, int>> added, removed;
  std::vector<bool> leftHidden;

protected:
  void create() override
  {
    registerComponent<Position>();
    registerComponent<Excluding<Hidden>>();
    observe<Any<Velocity, Health>>(
        [this](const Entity& entity) { added.emplace_back(entity.getId(), 1); },
        [this](const Entity& entity) { left(entity, 1); });
  }
  void onEntityAdded(const Entity& entity) override
  {
    added.emplace_back(entity.getId(), 0);
  }
  void onEntityRemoved(const Entity& entity) override { left(entity, 0); }

private:
  void left(const Entity& entity, int which)
  {
    removed.emplace_back(entity.getId(), which);
    leftHidden.push_back(entity.hasComponent<Hidden>());
  }
};

// Registers two Any groups: the second should be ignored.
class TwoAnySystem : public System
{
protected:
  void create() override
  {
    registerComponent<Any<Velocity, Health>>();
    registerComponent<Any<Enemy, Hidden>>();
  }
};

// Works on entities without Hidden and observes those with it, logging each
// hook and callback in the order they ran ("shown+", "shown-", "hidden+",
// "hidden-").
class HiddenWatcher : public System
{
public:
  std::vector<std::string> log;

protected:
  void create() override
  {
    registerComponent<Excluding<Hidden>>();
    observe<Hidden>([this](const Entity&) { log.push_back("hidden+"); },
                    [this](const Entity&) { log.push_back("hidden-"); });
  }
  void onEntityAdded(const Entity&) override { log.push_back("shown+"); }
  void onEntityRemoved(const Entity&) override { log.push_back("shown-"); }
};

// The ids in `list`, in its order.
std::vector<Entity::EntityId> idsOf(const std::vector<Entity>& list)
{
  std::vector<Entity::EntityId> ids;
  ids.reserve(list.size());
  for (const Entity& entity : list)
    ids.push_back(entity.getId());
  return ids;
}

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

  TEST("query should match entities having all requested components")
  {
    Registry reg;
    Entity moving = reg.createEntity();
    moving.addComponent<Position>();
    moving.addComponent<Velocity>();

    Entity still = reg.createEntity();
    still.addComponent<Position>();

    Entity hurt = reg.createEntity();
    hurt.addComponent<Health>();

    reg.flushEntities();
    CHECK(reg.query<Position>().size() == 2);           // moving + still
    CHECK(reg.query<Position, Velocity>().size() == 1); // moving only
    CHECK(reg.query<Health>().size() == 1);
    CHECK(reg.query<Velocity, Health>().empty());
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

  TEST("query should drop a destroyed entity at the flush")
  {
    Registry reg;
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    reg.flushEntities();
    CHECK(reg.query<Position>().size() == 1);
    reg.destroyEntity(e);
    CHECK(reg.query<Position>().size() == 1); // (until the flush)
    reg.flushEntities();
    CHECK(reg.query<Position>().empty());
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

  TEST("a system looked up while missing should be found once it's added")
  {
    Registry reg;
    CHECK(reg.tryGetSystem<MovementSystem>() == nullptr);
    CHECK(reg.tryGetSystem<MovementSystem>() == nullptr); // asked again
    MovementSystem& first = reg.addSystem<MovementSystem>();
    CHECK(reg.tryGetSystem<MovementSystem>() == &first);
    CHECK(reg.tryGetSystem<MovementSystem>() == &first);

    reg.removeSystem<MovementSystem>();
    CHECK(reg.tryGetSystem<MovementSystem>() == nullptr);
    MovementSystem& second = reg.addSystem<MovementSystem>();
    CHECK(reg.tryGetSystem<MovementSystem>() == &second);
    CHECK(&reg.getSystem<MovementSystem>() == &second);
  }

  TEST("a system should be found by a class it derives from, first added first")
  {
    class Base : public System
    {
    };
    class First : public Base
    {
    };
    class Second : public Base
    {
    };
    Registry reg;
    CHECK(reg.tryGetSystem<Base>() == nullptr);
    Second& second = reg.addSystem<Second>();
    CHECK(reg.tryGetSystem<Base>() == &second);
    First& first = reg.addSystem<First>();
    CHECK(reg.tryGetSystem<Base>() == &second);
    CHECK(reg.tryGetSystem<First>() == &first);
    CHECK(reg.tryGetSystem<System>() == &second);

    reg.removeSystem<Second>();
    CHECK(reg.tryGetSystem<Base>() == &first);
    CHECK(reg.tryGetSystem<Second>() == nullptr);
    reg.removeSystem<Base>(); // every system that is one
    CHECK(reg.tryGetSystem<First>() == nullptr);
    CHECK(!reg.hasSystem<System>());
  }

  TEST("registries should each answer for their own systems")
  {
    Registry with;
    Registry without;
    MovementSystem& sys = with.addSystem<MovementSystem>();
    CHECK(with.tryGetSystem<MovementSystem>() == &sys);
    CHECK(without.tryGetSystem<MovementSystem>() == nullptr);
    CHECK(with.tryGetSystem<MovementSystem>() == &sys);
  }

  TEST("two system types sharing a name should still be told apart in a "
       "debug build")
  {
    // Local classes of one name in one function share a name, so a lookup
    // id: the remembered answer is the other's, which a debug build catches.
    // It runs before the test that uses up the lookup's slots.
    Registry reg;
    System* added = nullptr;
    {
      class Twin : public System
      {
      };
      added = &reg.addSystem<Twin>();
      CHECK(reg.tryGetSystem<Twin>() == added);
    }
    {
      class Twin : public System
      {
      public:
        int other = 0;
      };
      if (debugFeatures::enabled())
      {
        CHECK(reg.tryGetSystem<Twin>() == nullptr);
        CHECK(!reg.hasSystem<Twin>());
      }
    }
  }

  TEST("a type's lookup id should be its own and the same every time")
  {
    const int movement = detail::systemLookupId<MovementSystem>();
    CHECK(movement == detail::systemLookupId<MovementSystem>());
    CHECK(movement != detail::systemLookupId<WatchingSystem>());
    CHECK(detail::systemTypeId(detail::componentTypeKey<MovementSystem>()) ==
          movement);
  }

  TEST("systems of more types than the lookup has slots should all be found")
  {
    Registry reg;
    std::vector<System*> added;
    addNumberedSystems(reg, added, std::make_integer_sequence<int, 300>());
    CHECK(added.size() == 300u);
    CHECK(findsNumberedSystems(
        reg, added, std::make_integer_sequence<int, 300>()));
    // Asked again: the remembered answers, and the scans past the slots.
    CHECK(findsNumberedSystems(
        reg, added, std::make_integer_sequence<int, 300>()));
    reg.removeSystem<NumberedSystem<299>>();
    reg.removeSystem<NumberedSystem<0>>();
    CHECK(reg.tryGetSystem<NumberedSystem<299>>() == nullptr);
    CHECK(reg.tryGetSystem<NumberedSystem<0>>() == nullptr);
    CHECK(reg.tryGetSystem<NumberedSystem<1>>() == added[1]);
  }

  TEST("a system looked up as it's removed should not be found after")
  {
    // Its destructor looks it up while it's still being taken out (as the
    // scan always has, it may find itself then): that answer mustn't stay.
    class Leaving : public System
    {
    public:
      ~Leaving() override
      {
        static_cast<void>(registry->tryGetSystem<Leaving>());
        static_cast<void>(registry->tryGetSystem<MovementSystem>());
      }
    };
    Registry reg;
    Leaving& leaving = reg.addSystem<Leaving>();
    MovementSystem& movement = reg.addSystem<MovementSystem>();
    CHECK(reg.tryGetSystem<Leaving>() == &leaving);
    reg.removeSystem<Leaving>();
    CHECK(reg.tryGetSystem<Leaving>() == nullptr);
    CHECK(!reg.hasSystem<Leaving>());
    CHECK(reg.tryGetSystem<MovementSystem>() == &movement);
  }

  TEST("in teardown a system should find no other, even one looked up before")
  {
    struct Seen
    {
      bool movementFound = true;
      bool selfFound = true;
    } seen;
    class Last : public System
    {
    public:
      Seen* seen = nullptr;
      ~Last() override
      {
        seen->movementFound =
            registry->tryGetSystem<MovementSystem>() != nullptr;
        seen->selfFound = registry->hasSystem<Last>();
      }
    };
    {
      Registry reg;
      MovementSystem& movement = reg.addSystem<MovementSystem>();
      Last& last = reg.addSystem<Last>();
      last.seen = &seen;
      CHECK(reg.tryGetSystem<MovementSystem>() == &movement); // remembered
      CHECK(reg.tryGetSystem<Last>() == &last);
    }
    CHECK(!seen.movementFound);
    CHECK(!seen.selfFound);
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

  TEST("componentChanges should count gaining and losing a component, not "
       "changing it")
  {
    Registry reg;
    const uint64_t before = reg.componentChanges<Velocity>();
    Entity a = reg.createEntity();
    a.addComponent<Position>();
    CHECK(reg.componentChanges<Velocity>() == before); // another type
    a.addComponent<Velocity>();
    const uint64_t gained = reg.componentChanges<Velocity>();
    CHECK(gained != before);
    reg.flushEntities();
    a.getComponent<Velocity>().dx = 3.0f;
    a.addComponent<Velocity>(); // (replaced: still has one)
    CHECK(reg.componentChanges<Velocity>() == gained);
    a.removeComponent<Velocity>();
    const uint64_t lost = reg.componentChanges<Velocity>();
    CHECK(lost != gained);
    a.addComponent<Velocity>();
    const uint64_t again = reg.componentChanges<Velocity>();
    CHECK(again != lost);
    reg.destroyEntity(a);
    CHECK(reg.componentChanges<Velocity>() == again); // (until the flush)
    reg.flushEntities();
    CHECK(reg.componentChanges<Velocity>() != again);
  }

  TEST("singleton should find the one entity with a component, kept up to "
       "date as it comes and goes")
  {
    Registry reg;
    CHECK(reg.singleton<Health>() == nullptr);
    Entity a = reg.createEntity();
    a.addComponent<Position>();
    CHECK(reg.singleton<Health>() == nullptr);
    a.addComponent<Health>();
    CHECK(reg.singleton<Health>() == &a.getComponent<Health>());
    reg.flushEntities();
    CHECK(reg.singleton<Health>() == &a.getComponent<Health>());

    // Several: one of them, still.
    Entity b = reg.createEntity();
    b.addComponent<Health>();
    Health* either = reg.singleton<Health>();
    CHECK(either == &a.getComponent<Health>() ||
          either == &b.getComponent<Health>());

    // The remembered one loses it: the other is found.
    a.removeComponent<Health>();
    CHECK(reg.singleton<Health>() == &b.getComponent<Health>());

    // Destroyed: gone once the flush takes it.
    reg.destroyEntity(b);
    reg.flushEntities();
    CHECK(reg.singleton<Health>() == nullptr);
    a.addComponent<Health>();
    CHECK(reg.singleton<Health>() == &a.getComponent<Health>());
  }

  TEST("query should list the entities with all its components in id order, "
       "kept up to date, the same list each time")
  {
    Registry reg;
    Entity a = reg.createEntity();
    Entity b = reg.createEntity();
    Entity c = reg.createEntity();
    a.addComponent<Position>();
    a.addComponent<Velocity>();
    c.addComponent<Position>();
    c.addComponent<Velocity>();
    b.addComponent<Position>();
    const std::vector<Entity>& movers = reg.query<Position, Velocity>();
    CHECK(movers.empty()); // pending: they join at the flush
    reg.flushEntities();
    CHECK(movers.size() == 2u);
    CHECK(movers[0].getId() == a.getId());
    CHECK(movers[1].getId() == c.getId());
    CHECK(&reg.query<Velocity, Position>() == &movers); // (cached)

    // A list first asked for after the flush is filled by its scan.
    CHECK(reg.query<Position>().size() == 3u);

    // Joining in the middle keeps id order; leaving drops it, at once.
    b.addComponent<Velocity>();
    CHECK(movers.size() == 3u);
    CHECK(movers[1].getId() == b.getId());
    a.removeComponent<Velocity>();
    CHECK(movers.size() == 2u);
    CHECK(movers[0].getId() == b.getId());

    // Destroyed: gone once the flush takes it.
    reg.destroyEntity(c);
    CHECK(movers.size() == 2u);
    reg.flushEntities();
    CHECK(movers.size() == 1u);
    CHECK(movers[0].getId() == b.getId());

    // A reused id joins in id order, not at the end.
    Entity d = reg.createEntity();
    CHECK(d.getId() == c.getId());
    d.addComponent<Position>();
    d.addComponent<Velocity>();
    reg.flushEntities();
    CHECK(movers.size() == 2u);
    CHECK(movers[0].getId() == b.getId());
    CHECK(movers[1].getId() == d.getId());
  }

  TEST("an entity made and destroyed before a flush should join no query")
  {
    Registry reg;
    const std::vector<Entity>& positioned = reg.query<Position>();
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    reg.destroyEntity(e);
    reg.flushEntities();
    CHECK(positioned.empty());
  }

  TEST("a system's entities should be the registry's query for its components, "
       "in id order")
  {
    Registry reg;
    MovementSystem& movement = reg.addSystem<MovementSystem>();
    std::vector<Entity> made;
    for (int i = 0; i < 4; ++i)
    {
      Entity e = reg.createEntity();
      e.addComponent<Position>();
      e.addComponent<Velocity>();
      made.push_back(e);
    }
    reg.flushEntities();
    CHECK(&movement.getEntities() == &reg.query<Position, Velocity>());
    reg.destroyEntity(made[0]);
    reg.flushEntities();
    made[1].removeComponent<Velocity>();
    made[1].addComponent<Velocity>(); // back in, before the others by id
    const std::vector<Entity>& movers = movement.getEntities();
    CHECK(movers.size() == 3u);
    CHECK(movers[0] == made[1] && movers[1] == made[2] && movers[2] == made[3]);
  }

  TEST("a system should be told as entities join and leave its entities and "
       "what it observes")
  {
    Registry reg;
    WatchingSystem& sys = reg.addSystem<WatchingSystem>();

    Entity mover = reg.createEntity();
    mover.addComponent<Position>();
    mover.addComponent<Velocity>();
    Entity hurt = reg.createEntity();
    hurt.addComponent<Health>();
    CHECK(sys.added.empty()); // (until the flush)
    reg.flushEntities();
    CHECK(sys.added.size() == 2u);
    CHECK(sys.added[0] == std::make_pair(mover.getId(), 0));
    CHECK(sys.added[1] == std::make_pair(hurt.getId(), 1));
    CHECK(sys.getEntities().size() == 1u);
    CHECK(reg.query<Health>().size() == 1u);
    CHECK(reg.query<Health>().front().getId() == hurt.getId());

    // Gaining Health: the observer is told.
    mover.addComponent<Health>();
    CHECK(sys.added.size() == 3u);
    CHECK(sys.added.back() == std::make_pair(mover.getId(), 1));

    // Losing Velocity: it leaves the system's entities only.
    mover.removeComponent<Velocity>();
    CHECK(sys.removed.size() == 1u);
    CHECK(sys.removed.back() == std::make_pair(mover.getId(), 0));
    CHECK(sys.leftWithPosition.back());
    CHECK(sys.getEntities().empty());

    // Destroyed: it leaves what it's still in, its components still there.
    reg.destroyEntity(mover);
    reg.flushEntities();
    CHECK(sys.removed.size() == 2u);
    CHECK(sys.removed.back() == std::make_pair(mover.getId(), 1));
    CHECK(sys.leftWithPosition.back());
    CHECK(reg.query<Health>().size() == 1u);
  }

  TEST("hooks should run after every list is updated, by system registration "
       "order, the entities' hooks before the observe callbacks")
  {
    Registry reg;
    std::vector<std::string> log;
    HealthCounter& counter = reg.addSystem<HealthCounter>();
    counter.log = &log;
    WatchingSystem& first = reg.addSystem<WatchingSystem>();
    first.log = &log;

    Entity e = reg.createEntity();
    e.addComponent<Position>();
    e.addComponent<Velocity>();
    e.addComponent<Health>();
    reg.flushEntities();
    CHECK(log ==
          std::vector<std::string>({"counter+", "watching+0", "watching+1"}));
    CHECK(counter.joined == 1);
    CHECK(counter.listedWhenTold);

    // Destroyed: told in the same order, still with its components.
    log.clear();
    reg.destroyEntity(e);
    reg.flushEntities();
    CHECK(log == std::vector<std::string>({"watching-0", "watching-1"}));
    CHECK(first.leftWithPosition.size() == 2u);
    CHECK(first.leftWithPosition[0] && first.leftWithPosition[1]);
  }

  TEST("a system added late should be told of the entities already there, in "
       "id order, but not of those pending a flush")
  {
    Registry reg;
    Entity healer = reg.createEntity();
    healer.addComponent<Health>();
    Entity mover = reg.createEntity();
    mover.addComponent<Position>();
    mover.addComponent<Velocity>();
    mover.addComponent<Health>();
    reg.flushEntities();
    Entity pending = reg.createEntity();
    pending.addComponent<Health>();

    WatchingSystem& sys = reg.addSystem<WatchingSystem>();
    CHECK(sys.added.size() == 3u);
    CHECK(sys.added[0] == std::make_pair(healer.getId(), 1));
    CHECK(sys.added[1] == std::make_pair(mover.getId(), 0));
    CHECK(sys.added[2] == std::make_pair(mover.getId(), 1));
    CHECK(sys.getEntities().size() == 1u);
    reg.flushEntities();
    CHECK(sys.added.size() == 4u);
    CHECK(sys.added.back() == std::make_pair(pending.getId(), 1));
  }

  TEST("a removed system should no longer be told")
  {
    Registry reg;
    HealthCounter& counter = reg.addSystem<HealthCounter>();
    Entity e = reg.createEntity();
    e.addComponent<Health>();
    reg.flushEntities();
    CHECK(counter.joined == 1);
    reg.removeSystem<HealthCounter>();
    reg.createEntity().addComponent<Health>();
    reg.flushEntities(); // (counter is gone: nothing may call it)
    CHECK(reg.query<Health>().size() == 2u);
  }

  TEST("a system's destructor should still read components and queries")
  {
    struct Seen
    {
      int hp = -1;
      std::size_t listed = 0;
      bool systemFound = true;
    } seen;
    class Flusher : public System
    {
    public:
      Seen* seen = nullptr;
      Entity watched;
      ~Flusher() override
      {
        seen->hp = watched.getComponent<Health>().hp;
        seen->listed = registry->query<Health>().size();
        seen->systemFound = registry->tryGetSystem<Flusher>() != nullptr;
      }
    };
    {
      Registry reg;
      Entity e = reg.createEntity();
      e.addComponent<Health>(42);
      reg.createEntity().addComponent<Health>();
      reg.flushEntities();
      Flusher& flusher = reg.addSystem<Flusher>();
      flusher.seen = &seen;
      flusher.watched = e;
    }
    CHECK(seen.hp == 42);
    CHECK(seen.listed == 2u);
    CHECK(!seen.systemFound);
  }

  TEST("an Any query should list entities with at least one of its "
       "components, moving them in and out as they gain and lose them")
  {
    Registry reg;
    Entity a = reg.createEntity();
    Entity b = reg.createEntity();
    Entity c = reg.createEntity();
    Entity d = reg.createEntity();
    a.addComponent<Position>();
    b.addComponent<Velocity>();
    c.addComponent<Health>();
    d.addComponent<Position>();
    d.addComponent<Velocity>();
    const std::vector<Entity>& either = reg.query<Any<Position, Velocity>>();
    CHECK(either.empty()); // pending: they join at the flush
    reg.flushEntities();
    using Ids = std::vector<Entity::EntityId>;
    CHECK(idsOf(either) == Ids({a.getId(), b.getId(), d.getId()}));

    // Losing the only one leaves; losing one of two stays; gaining one joins
    // in id order.
    a.removeComponent<Position>();
    d.removeComponent<Position>();
    c.addComponent<Velocity>();
    CHECK(idsOf(either) == Ids({b.getId(), c.getId(), d.getId()}));

    // Destroyed: gone once the flush takes it.
    reg.destroyEntity(b);
    CHECK(either.size() == 3u);
    reg.flushEntities();
    CHECK(idsOf(either) == Ids({c.getId(), d.getId()}));
  }

  TEST("an Excluding query should drop an entity that gains an excluded "
       "component and take it back when it loses it")
  {
    Registry reg;
    Entity a = reg.createEntity();
    Entity b = reg.createEntity();
    a.addComponent<Position>();
    b.addComponent<Position>();
    b.addComponent<Hidden>();
    const std::vector<Entity>& shown = reg.query<Position, Excluding<Hidden>>();
    reg.flushEntities();
    using Ids = std::vector<Entity::EntityId>;
    CHECK(idsOf(shown) == Ids({a.getId()}));

    b.removeComponent<Hidden>();
    CHECK(idsOf(shown) == Ids({a.getId(), b.getId()}));
    a.addComponent<Hidden>();
    CHECK(idsOf(shown) == Ids({b.getId()}));
    a.removeComponent<Position>(); // hidden and unpositioned: still out
    a.removeComponent<Hidden>();
    CHECK(idsOf(shown) == Ids({b.getId()}));

    // Excluding alone: everything flushed without the component.
    CHECK(idsOf(reg.query<Excluding<Hidden>>()) == Ids({a.getId(), b.getId()}));

    reg.destroyEntity(b);
    CHECK(shown.size() == 1u);
    reg.flushEntities();
    CHECK(shown.empty());
  }

  TEST("a query should mix all-of, Any and Excluding, Excluding winning")
  {
    Registry reg;
    std::vector<Entity> made;
    for (int i = 0; i < 5; ++i)
    {
      made.push_back(reg.createEntity());
      made.back().addComponent<Position>();
    }
    // 0 and 1 match; 2 is excluded; 3 has no Any and is excluded; 4 has no
    // Position.
    made[0].addComponent<Velocity>();
    made[1].addComponent<Health>();
    made[2].addComponent<Velocity>();
    made[2].addComponent<Enemy>();
    made[3].addComponent<Hidden>();
    made[4].removeComponent<Position>();
    made[4].addComponent<Velocity>();
    reg.flushEntities();
    const std::vector<Entity>& mixed =
        reg.query<Position, Any<Velocity, Health>, Excluding<Enemy, Hidden>>();
    using Ids = std::vector<Entity::EntityId>;
    CHECK(idsOf(mixed) == Ids({made[0].getId(), made[1].getId()}));

    made[2].removeComponent<Enemy>();
    made[3].addComponent<Health>(); // has one of Any, but still Hidden
    made[4].addComponent<Position>();
    CHECK(idsOf(mixed) == Ids({made[0].getId(),
                               made[1].getId(),
                               made[2].getId(),
                               made[4].getId()}));
    made[0].addComponent<Hidden>();
    CHECK(idsOf(mixed) ==
          Ids({made[1].getId(), made[2].getId(), made[4].getId()}));

    // Excluding wins over all-of and over Any.
    CHECK(reg.query<Position, Excluding<Position>>().empty());
    CHECK(reg.query<Any<Position, Velocity>, Excluding<Position, Velocity>>()
              .empty());
  }

  TEST("queries that differ only in their filters should keep separate "
       "lists; the same terms in any order should share one")
  {
    Registry reg;
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    e.addComponent<Velocity>();
    reg.flushEntities();
    const std::vector<const std::vector<Entity>*> lists = {
        &reg.query<Position, Velocity>(),
        &reg.query<Position, Any<Velocity, Health>>(),
        &reg.query<Position, Excluding<Velocity>>(),
        &reg.query<Any<Position, Velocity>>(),
        &reg.query<Excluding<Position, Velocity>>(),
        &reg.query<Position>()};
    for (std::size_t i = 0; i < lists.size(); ++i)
      for (std::size_t j = i + 1; j < lists.size(); ++j)
        CHECK(lists[i] != lists[j]);
    CHECK(lists[0]->size() == 1u && lists[1]->size() == 1u);
    CHECK(lists[2]->empty() && lists[4]->empty());

    CHECK(&reg.query<Excluding<Hidden>, Position>() ==
          &reg.query<Position, Excluding<Hidden>>());
    CHECK(&reg.query<Any<Velocity, Health>, Position>() ==
          &reg.query<Position, Any<Health, Velocity>>());
    CHECK(&reg.query<Excluding<Hidden>, Position, Excluding<Enemy>>() ==
          &reg.query<Position, Excluding<Enemy, Hidden>>());
  }

  TEST("queries that always list the same entities should share one list")
  {
    Registry reg;
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    e.addComponent<Velocity>();
    reg.flushEntities();
    // An Any of one component is that component.
    CHECK(&reg.query<Position, Any<Velocity>>() ==
          &reg.query<Position, Velocity>());
    CHECK(&reg.query<Any<Position>>() == &reg.query<Position>());
    // An Any sharing a component with the all-of terms is always met.
    CHECK(&reg.query<Position, Any<Position, Health>>() ==
          &reg.query<Position>());
    // An Any of one excluded component can't be met: still nothing listed.
    CHECK(reg.query<Any<Velocity>, Excluding<Velocity>>().empty());
    CHECK(reg.query<Any<Velocity, Health>, Excluding<Velocity, Health>>()
              .empty());
    CHECK(reg.query<Position, Any<Velocity>>().size() == 1u);
  }

  TEST("a loop that adds a component its own query excludes should go "
       "through a copy of the list")
  {
    Registry reg;
    for (int i = 0; i < 4; ++i)
      reg.createEntity().addComponent<Position>();
    reg.flushEntities();
    const std::vector<Entity>& shown = reg.query<Position, Excluding<Hidden>>();
    for (Entity entity : std::vector<Entity>(shown))
      entity.addComponent<Hidden>();
    CHECK(shown.empty());
    CHECK(reg.query<Hidden>().size() == 4u);
  }

  TEST("one change that moves an entity out of a system's list and into its "
       "observer's should tell it in list order")
  {
    Registry reg;
    HiddenWatcher& watcher = reg.addSystem<HiddenWatcher>();
    Entity e = reg.createEntity();
    reg.flushEntities();
    e.addComponent<Hidden>();
    e.removeComponent<Hidden>();
    CHECK(watcher.log ==
          std::vector<std::string>(
              {"shown+", "shown-", "hidden+", "shown+", "hidden-"}));
  }

  TEST("filtered queries should join and leave at the same flushes as all-of "
       "queries")
  {
    Registry reg;
    const std::vector<Entity>& positioned = reg.query<Position>();
    const std::vector<Entity>& shown = reg.query<Position, Excluding<Hidden>>();
    const std::vector<Entity>& either = reg.query<Any<Position, Velocity>>();

    // Made and destroyed before a flush: in none.
    Entity gone = reg.createEntity();
    gone.addComponent<Position>();
    reg.destroyEntity(gone);
    // Pending: joins at the flush, as it is then.
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    e.addComponent<Hidden>();
    e.removeComponent<Hidden>();
    CHECK(positioned.empty() && shown.empty() && either.empty());
    reg.flushEntities();
    CHECK(positioned.size() == 1u && shown.size() == 1u && either.size() == 1u);
    CHECK(shown.front() == e && either.front() == e);

    reg.destroyEntity(e);
    CHECK(positioned.size() == 1u && shown.size() == 1u && either.size() == 1u);
    reg.flushEntities();
    CHECK(positioned.empty() && shown.empty() && either.empty());
  }

  TEST("a system's own terms and observers should take Any and Excluding, its "
       "hooks running as entities move in and out")
  {
    Registry reg;
    VisibleSystem& sys = reg.addSystem<VisibleSystem>();
    CHECK(&sys.getEntities() == &reg.query<Position, Excluding<Hidden>>());

    Entity e = reg.createEntity();
    e.addComponent<Position>();
    e.addComponent<Health>();
    reg.flushEntities();
    using Joins = std::vector<std::pair<Entity::EntityId, int>>;
    CHECK(sys.added == Joins({{e.getId(), 0}, {e.getId(), 1}}));

    // Gaining the excluded component leaves, already having it; losing it
    // joins again.
    e.addComponent<Hidden>();
    CHECK(sys.removed == Joins({{e.getId(), 0}}));
    CHECK(sys.leftHidden.back());
    CHECK(sys.getEntities().empty());
    e.removeComponent<Hidden>();
    CHECK(sys.added.back() == std::make_pair(e.getId(), 0));
    CHECK(sys.getEntities().size() == 1u);

    // The observer's Any: gaining a second keeps it, losing both leaves.
    e.addComponent<Velocity>();
    e.removeComponent<Health>();
    CHECK(sys.removed.size() == 1u);
    e.removeComponent<Velocity>();
    CHECK(sys.removed.back() == std::make_pair(e.getId(), 1));

    // A second Any group is ignored.
    TwoAnySystem& twoAny = reg.addSystem<TwoAnySystem>();
    QuerySignature firstAny;
    firstAny.any.set(Component<Velocity>::getId());
    firstAny.any.set(Component<Health>::getId());
    CHECK(twoAny.getComponentSignature() == firstAny);
    CHECK(&twoAny.getEntities() == &reg.query<Any<Velocity, Health>>());
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
