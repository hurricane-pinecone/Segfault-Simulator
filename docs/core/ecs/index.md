# Entities, components & systems

The core of SegFaultSimulator is a small **entity-component-system** (ECS), with an
optional Unity-style **GameObject** layer on top. You'll use both: ECS for
data-driven behaviour over many entities, GameObjects for individually scripted
actors.

## The model

- **Entity** — a lightweight id. It holds no data itself; you attach components.
- **Component** — a plain data struct (position, sprite, health, …): the entity's
  state, including what changes as the game runs. No behaviour.
- **System** — behaviour that runs each frame over every entity carrying a chosen
  set of components.

You create entities and systems through the **`Scene`** (your level/screen), and
mutate components through the **`Entity`** handle. The engine's `Registry` owns the
storage. In a Scene-based game, your code never touches it directly: entities and
systems go through `Scene`, and a `System` subclass reaches it through its protected
`registry` pointer. For headless contexts (a tool, a test, or a host that brings its
own rendering), you can construct and use `Registry` directly without the runtime.

## Components

A component is any struct — define your own:

```cpp
struct Health   { int current = 100; int max = 100; };
struct Velocity { glm::vec2 value{0.0f, 0.0f}; };
```

The engine also ships components such as `TransformComponent` (position / scale /
rotation), `SpriteComponent`, `ParticleEmitterComponent`, and `NameComponent` (an
optional display name, useful for lookups and for showing entities in a tool's
hierarchy view). Up to 128 distinct component types can be registered across the
engine and your game (`MAX_COMPONENTS`).

## Entities

Spawn through the scene, then attach components via the returned `Entity`
(`addComponent` returns the entity, so calls chain):

```cpp
sfs::Entity e = scene.createEntity();
e.addComponent<sfs::TransformComponent>(glm::vec2{4.0f, 2.0f});
e.addComponent<Health>();
e.addComponent<Velocity>();

if (e.hasComponent<Health>())
  e.getComponent<Health>().current -= 10;

e.removeComponent<Velocity>();
scene.destroyEntity(e);
```

`addTag<T>()` is `addComponent<T>()` read more naturally for empty marker
components — e.g. `e.addTag<Player>()`.

A reference from `getComponent` stays good while other entities get
components (each type's storage grows in pages that never move), until that
component is removed or its entity destroyed. `removeComponent` destroys the
component, and the entity leaves (or, adding one, joins) the systems that no
longer (or now) match it.

An `Entity` is a handle: an id and a generation. Once its entity is destroyed
(at the next flush) `isAlive()` is false, even after a new entity reuses the
id, and `hasComponent` is false. Reading or changing the components of an
entity that isn't alive, or of a default-made `Entity`, is a fatal error (see
below) naming the component and why, instead of reading another entity's data.

`scene.entities()` returns every living entity, in id order (`registry.entities()`
in headless use). This is handy for a tool that needs to list every entity
regardless of which components it carries, such as a hierarchy panel.

## Systems

A system declares which components it cares about and runs logic over the matching
entities each frame. Subclass `System`, register its components in `create()`, and
do the work in `update()`:

```cpp
class DriftSystem : public sfs::System
{
protected:
  void create() override
  {
    registerComponent<sfs::TransformComponent>();
    registerComponent<Velocity>();
  }

  void update(double dt) override
  {
    for (const sfs::Entity& e : getEntities()) // entities with BOTH components
    {
      auto& t       = e.getComponent<sfs::TransformComponent>();
      const auto& v = e.getComponent<Velocity>();
      t.setPosition2D(t.position2D() + v.value * static_cast<float>(dt));
    }
  }
};
```

`getEntities()` is the system's own membership: every entity whose components
include the set it registered. For an ad-hoc query across a different component
set, a system can also use `registry->view<A, B, ...>()`.

A system has three optional lifecycle hooks: `create()` (once — register
components), `update(double dt)` (per frame), and `render()` (per frame, after
update). Add it to a scene and fetch it later:

```cpp
scene.addSystem<DriftSystem>();

DriftSystem& drift = scene.getSystem<DriftSystem>(); // system must be present
if (scene.hasSystem<DriftSystem>()) { /* ... */ }

// When presence is conditional, use tryGetSystem -- it returns null if absent.
if (DriftSystem* opt = scene.tryGetSystem<DriftSystem>())
{
  // ...
}
```

`getSystem<T>()` asserts the system was registered. Calling it for an absent
system is a programming error: a fatal error (`sfs::fatalError`, which logs
it and throws `sfs::FatalError`) naming the missing type. The game loop
catches it, tells the player in a message box and ends the game. Use
`tryGetSystem<T>()` when a system may or may not be present.

`scene.view<A, B>()` makes a list; `registry->each<A, B>(fn)` (in a system)
calls `fn(entity, a, b)` for each match without one, for code that runs every
frame.

`registry->componentChanges<A>()` goes up whenever an entity gains or loses an
`A` (a destroyed entity loses all of its components). A system that keeps its
own list of the entities with an `A` searches again only when it has moved,
instead of every frame. Changing an `A`'s values doesn't count: compare those
yourself.

### Joining and leaving

All per-entity data belongs on its components (hidden fields for what isn't
authored), which systems update: a system never keeps its own map or struct per
entity. A system that must set something up or release it as an entity joins or
leaves (an engine-side resource, a shared index) does it in
`onEntityAdded(entity, set)` and `onEntityRemoved(entity, set)`, rather than
searching `getEntities()` for newcomers every frame. They're called as an entity joins or leaves: when it
gains or loses a component, when it's created or destroyed (at the registry's
flush), and when the system is added to a registry that already has it.
Joining, it has all the set's components. Leaving, it's still alive with its
other components: a destroyed entity still has all of them, and one that lost
a component no longer has that one. Keep the hooks to the system's own state:
don't add or remove components, entities or systems from them.

A system that also works on a second kind of entity watches it as another set,
registered where its components are:

```cpp
class RigSystem : public sfs::System
{
  int m_tools = -1;
  std::unordered_map<sfs::Entity::EntityId, Rig> m_rigs;
  std::unordered_map<sfs::Entity::EntityId, Tool> m_toolStates;

protected:
  void create() override
  {
    registerComponent<sfs::VehicleComponent>(); // set 0: getEntities()
    m_tools = watch<Implement, sfs::TransformComponent>();
  }
  void onEntityAdded(const sfs::Entity& e, int set) override
  {
    if (set == 0)
      m_rigs.emplace(e.getId(), Rig{});
    else
      m_toolStates.emplace(e.getId(), Tool{});
  }
  void onEntityRemoved(const sfs::Entity& e, int set) override
  {
    if (set == 0)
      m_rigs.erase(e.getId());
    else
      m_toolStates.erase(e.getId());
  }
};
```

`getEntities(set)` lists a watched set's entities.

A program can have at most `MAX_COMPONENTS` (128) component types; one more is
a fatal error when it's first used.

`setEnabled(false)` skips a system's update/render (handy for debug toggles).

### Game time

Systems run in **game time**, which can go faster or slower than real time:
`registry->setTimeScale(4.0)` (from a system) makes every system's `dt` four
times the frame's, for a fast-forward. A big `dt` can make what steers or
follows a path overshoot, so a scene can split a frame's game time into steps:
after `registry->setMaxTimeStep(1.0 / 30.0)`, a system is updated as many
times a frame as it takes for no step to be longer than that (at most 16 by
default; past that the game falls behind rather than take longer steps, so a
slow frame can't make the next one slower). Each system picks how it takes
the time with `setTimeStep()`:

| `System::TimeStep` | `update(dt)` gets |
| --- | --- |
| `Stepped` (default) | game time, in those steps: the same at any speed |
| `Frame` | the frame's game time in one step (counting time, or costly work) |
| `Real` | real time, whatever the scale (`RenderSystem`, `CameraSystem`) |

`AnimationSystem` is `Frame`. Game objects' `onUpdate()` and the scene's own
get real time: they're the player's (input, cameras). A `ParticleModule` is
simulated with the frame's real time times the registry's time scale, so its
particles keep game time too.

`registry->gameTime()` is the game seconds since the registry began updating:
the sum of the game time its systems have been given (a game's stopwatch,
Unity's `Time.time`). The scene adds each step's time before that step's
systems run, so a `Stepped` system reads the time at the end of its step, and
`Frame` and `Real` systems read the frame's end. Only the scene advances it.
Scripts read the same through `scene().gameTime()` and `scene().paused()`.

`registry->setPaused(true)` pauses the game: the engine's own moving parts,
`AnimationSystem`'s animators and `ParticleModule`'s particles, stand still.
Systems keep getting the game time the time scale gives them (and
`gameTime()` keeps counting); each one decides what a pause stops for it by
reading `registry->paused()`. Like the time scale, it takes effect from the
next frame.

## GameObjects — the OOP layer

For a single scripted actor (a player, a boss) the ECS-only style is verbose. A
**`GameObject`** wraps one entity behind an object you subclass:

```cpp
class Player : public sfs::GameObject
{
protected:
  void onCreate(sfs::Scene& scene) override
  {
    entity().addComponent<sfs::TransformComponent>();
    entity().addComponent<Health>();
  }

  void onUpdate(double dt) override { /* per-frame logic */ }
  void onProcessInput(const sfs::Input& input) override { /* ... */ }
};
```

Create it through the scene; `onCreate` fires immediately with that scene:

```cpp
Player& player = scene.createObject<Player>();
sfs::Entity& e = player.entity();           // the underlying entity
Player* p = scene.tryFindObject<Player>();   // find one later (or nullptr)
```

`scene.destroyObject(&player)` destroys an object and its entity. Inside an
update, input or contact callback it only marks it (it gets no more calls, and
isn't found); once they're done its `onDestroy()` runs and it's deleted.
Destroying an object's entity (`scene.destroyEntity`) destroys the object too,
at the next flush, and a scene's objects get `onDestroy()` when it ends.
Objects made during an update start getting updates next frame.

In a `StandardScene` (a scene file played), its `PrefabSystem`
(`spawn("asset:<id>", transform)`; see
[Prefab system](../../runtime/systems/prefab-system/index.md)) makes a prefab's
instance while the game runs and starts its scripts, as when the scene started.

Rule of thumb: use **systems** for behaviour over many homogeneous entities
(movement, physics, rendering); use **GameObjects** for individually scripted
actors. Both sit on the same entities, so they compose freely.

## Headless use

`Registry` is directly constructable, so the full runtime is not required. This
is the path for engine tools, stand-alone tests, or a custom host that provides
its own rendering:

```cpp
sfs::Registry reg;

sfs::Entity e = reg.createEntity();
e.addComponent<Position>(Position{1.0f, 2.0f});
e.addComponent<Velocity>(Velocity{0.0f, 1.0f});

reg.addSystem<DriftSystem>();    // create() runs immediately
reg.flushEntities();             // distributes new entities to matched systems

reg.destroyEntity(e);
reg.flushEntities();             // destruction is deferred until flush
```

Entity destruction is deferred: a destroyed entity stays alive (and its
components remain readable) until the next `flushEntities()` call. Views and
system membership update at flush time, so call `flushEntities()` whenever the
caller needs a consistent snapshot.
