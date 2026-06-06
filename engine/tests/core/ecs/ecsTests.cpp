// Tests for the core ECS: Entity lifecycle, component pools, queries (view),
// and system entity-matching. The Registry is constructed directly (no Scene),
// which exercises engine-core headless.

#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>

#include <cstddef>
#include <stdexcept>

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
  // --- Entity defaults ----------------------------------------------------
  {
    Entity none;
    CHECK(!none.isValid());
    CHECK(!static_cast<bool>(none));
    CHECK(none.getId() == Entity::InvalidId);
  }

  // --- create / alive / unique ids ----------------------------------------
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

  // --- Pool<T> directly ---------------------------------------------------
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

    // remove(const T&) clears the slot holding that value.
    pool.set(0, 7);
    CHECK(pool.has(0));
    pool.remove(7);
    CHECK(!pool.has(0));

    pool.clear();
    CHECK(pool.isEmpty());
  }

  // --- components: add / has / get / mutate / remove ----------------------
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

    // mutation through the returned reference persists
    e.getComponent<Position>().x = 9.0f;
    CHECK(testing::approx(e.getComponent<Position>().x, 9.0f));

    e.removeComponent<Position>();
    CHECK(!e.hasComponent<Position>());
    CHECK(e.hasComponent<Velocity>());
  }

  // --- per-entity component independence ----------------------------------
  {
    Registry reg;
    Entity a = reg.createEntity();
    Entity b = reg.createEntity();
    a.addComponent<Health>(Health{30});
    b.addComponent<Health>(Health{70});
    CHECK(a.getComponent<Health>().hp == 30);
    CHECK(b.getComponent<Health>().hp == 70);
  }

  // --- tags ---------------------------------------------------------------
  {
    Registry reg;
    Entity e = reg.createEntity();
    e.addTag<Enemy>();
    CHECK(e.hasComponent<Enemy>());
  }

  // --- view: matches entities having ALL requested components -------------
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
    CHECK(reg.view<Position, Velocity>().size() == 1);  // moving only
    CHECK(reg.view<Health>().size() == 1);
    CHECK(reg.view<Velocity, Health>().empty());
  }

  // --- destroy + flush: generation bump, id reuse, stale handle invalid ----
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

    // id is reused with a bumped generation, so the old handle no longer matches
    Entity b = reg.createEntity();
    CHECK(b.getId() == aid);
    CHECK(b.getGeneration() == gen + 1);
    CHECK(!reg.isAlive(a)); // stale handle stays dead
    CHECK(reg.isAlive(b));
  }

  // --- view drops a destroyed entity after flush --------------------------
  {
    Registry reg;
    Entity e = reg.createEntity();
    e.addComponent<Position>();
    CHECK(reg.view<Position>().size() == 1);
    reg.destroyEntity(e);
    reg.flushEntities();
    CHECK(reg.view<Position>().empty());
  }

  // --- systems: registration, lookup, removal -----------------------------
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

  // --- getSystem throws when absent ---------------------------------------
  {
    Registry reg;
    bool threw = false;
    try
    {
      reg.getSystem<MovementSystem>();
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    CHECK(threw);
  }

  // --- system entity-matching on flush ------------------------------------
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

    // destroying a matched entity removes it from the system on flush
    reg.destroyEntity(moving);
    reg.flushEntities();
    CHECK(sys.getEntities().empty());
  }

  // --- enabled flag -------------------------------------------------------
  {
    Registry reg;
    MovementSystem& sys = reg.addSystem<MovementSystem>();
    CHECK(sys.enabled());
    sys.setEnabled(false);
    CHECK(!sys.enabled());
  }

  return testing::report("ecsTests");
}
