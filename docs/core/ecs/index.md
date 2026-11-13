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

See [A system's entities](#a-systems-entities-registercomponent) for what
`getEntities()` holds, and [Queries](#queries-lists-of-entities-by-component) for
lists of other kinds of entity.

A system has four optional lifecycle hooks: `create()` (once — register
components), `update(double dt)` (per frame), `lateUpdate(double dt)` (per
frame, after the game objects; see [Late update](#late-update)) and
`render()` (per frame, after update). Add it to a scene and fetch it later:

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

A lookup finds the first system added that is a `T` (that type, or one
derived from it). The registry remembers the answer for each type until a
system is added or removed, so a lookup costs the same however many systems
the scene has, and wherever `T` is among them. Systems still don't read other
systems: lookups are for scripts, tools and setup.

- Any thread may look a system up, but add and remove systems only while no
  other thread is looking one up. A lookup that overlaps a change anyway
  remembers nothing.
- System types need distinct qualified names, as component types do: their
  ids come from their names. Two local classes of one name in one function,
  or anonymous-namespace classes of one name in different files, would share
  one. Debug builds log such a clash and fall back to a scan.

When the registry goes (a scene closing), its systems are destroyed first,
in the order they were added, while every entity, component and query still
exists: a system's destructor can read its components and queries to save or
flush what it leaves (as Unity's `OnDestroy` and Unreal's `EndPlay` run with
the world intact). Systems are no longer found then: `tryGetSystem()` returns
null.

`registry->componentChanges<A>()` goes up whenever an entity gains or loses an
`A` (a destroyed entity loses all of its components). Code that works
something out from the entities with an `A` can redo it only when it has
moved, instead of every frame. Changing an `A`'s values doesn't count: compare those
yourself.

### A system's entities: registerComponent

`registerComponent<T>()`, called in the system's constructor or `create()`,
adds `T` to the components an entity needs to be one of the system's
entities. `T` may also be a filter group, as a query takes them (see
[Any and Excluding](#any-and-excluding)):
`registerComponent<sfs::Excluding<Hidden>>()` leaves out hidden entities, and
`registerComponent<sfs::Any<A, B>>()` asks for one of a group (one `Any` group
per system: a second is logged and ignored). `getEntities()` returns them: the
living entities that match its registered terms, in id order. It is the
registry's shared query for those terms (see
[Queries](#queries-lists-of-entities-by-component)), so the system keeps no
copy, and it is read-only.

A new entity joins at the registry's flush (the start of the scene's next
update, or `flushEntities()`); an entity already flushed joins or leaves at
once as it gains or loses a component; a destroyed one leaves at the flush.

All per-entity data belongs on its components (hidden fields for what isn't
authored), which systems update: a system never keeps its own map or struct per
entity. A system that must set something up or release it as one of its
entities joins or leaves (an engine-side resource, a shared index) does it in
`onEntityAdded(entity)` and `onEntityRemoved(entity)`, rather than searching
`getEntities()` for newcomers every frame. They're called as an entity joins
or leaves: when it gains or loses a component, when it's created or destroyed
(at the flush), and, for every entity already there in id order, when the
system is added to a registry. Joining, it has all the registered components.
Leaving, it's still alive with its other components: a destroyed entity still
has all of them, one that lost a component no longer has that one, and one
that gained an excluded component already has it. Keep the hooks to the
system's own state: don't add or remove components, entities or systems from
them.

### Queries: lists of entities by component

`registry->query<A, B>()` (scripts: `scene().query<A, B>()`) is the list of
entities with both components, in id order: the living ones that have been
flushed, with the same timing as a system's entities. The registry makes it
the first time it's asked for (one scan) and keeps it up to date from then on
as components are added and removed, so reading it every frame costs nothing,
and everyone asking for the same components shares the list. The reference
stays valid as long as the registry. To add or remove those component types
while going through it, go through a copy (destroying entities is fine: they
leave at the next flush).

```cpp
for (const sfs::Entity& light :
     registry->query<sfs::TransformComponent, sfs::PointLightComponent>())
  draw(light);
```

Use a query for any other kind of entity a system or script reads; a system's
own kind is `getEntities()`.

#### Any and Excluding

A query's template arguments are terms. A plain component is one the entity
must have. `sfs::Any<A, B>` asks for at least one of a group, and
`sfs::Excluding<C, D>` for none of a group. They mix freely, in any order:

```cpp
// Workers and vehicles, wherever they are.
registry->query<sfs::TransformComponent, sfs::Any<Worker, sfs::VehicleComponent>>();
// Everything with a transform that isn't hidden.
registry->query<sfs::TransformComponent, sfs::Excluding<Hidden>>();
// All three: positioned, moving or hurt, and neither an enemy nor hidden.
registry->query<Position, sfs::Any<Velocity, Health>, sfs::Excluding<Enemy, Hidden>>();
```

- An entity with none of an `Any` group is not listed. A query takes at most
  one `Any` group (a second is a compile error), and an empty group is one too.
- `Excluding` wins over everything else: `query<A, sfs::Excluding<A>>()` is
  empty. Several `Excluding` groups add up.
- The same terms in another order are the same query and share its list, and
  so do terms that always list the same entities: an `Any` of one component
  is that component (`query<A, sfs::Any<B>>()` is `query<A, B>()`), and an
  `Any` sharing a component with the plain terms is always met
  (`query<A, sfs::Any<A, B>>()` is `query<A>()`). Terms that list different
  entities (`query<A, B>()`, `query<A, sfs::Any<B, C>>()`,
  `query<A, sfs::Excluding<B>>()`) each have a list of their own.
- A loop that adds or removes a component its own query names changes the
  list it is going through, and skips entities. Filters make this easy to
  write by accident: "mark everything not yet marked" is
  `query<A, sfs::Excluding<Marked>>()` plus `addComponent<Marked>()`. Go
  through a copy:

  ```cpp
  for (sfs::Entity entity :
       std::vector<sfs::Entity>(registry->query<A, sfs::Excluding<Marked>>()))
    entity.addComponent<Marked>();
  ```

These are the semantics of Unity DOTS (`WithAll`/`WithAny`/`WithNone`), Bevy
(`With`/`Or`/`Without`) and Flecs (`A, B || C, !D`). A filtered list is cached
and kept up to date exactly like an all-of one: no filtering as you read it,
the same flush timing, the same hooks. The one difference is which changes
move an entity: gaining an excluded component makes it leave a list, and
losing one makes it join.

#### What lists cost

The registry keeps each list's `sfs::QuerySignature` (three 128-bit sets: all,
any and excluding, 48 bytes) in one dense array, the lists themselves by the
same index, and, for each component type, the indices of the lists that name
it (4 bytes per list per component it names). It stores nothing per entity
beyond the lists. Reading a list is a hash lookup on the signature, which each
`query<...>()` builds once.

An entity gaining or losing a component is tested only against the lists that
name that component: two signature tests each, plus the sorted insert or
erase where it joins or leaves. Systems are told only when a list one of them
follows changed. A flush tests each entity joining or leaving against every
list, streaming the dense signatures. `ecsBenchTests` times this with 106
query lists, 60 systems and 2000 entities: a component no list names costs
about 20 ns to add or remove, one a few lists name about 0.7 µs, and one about
half the lists name about 3 µs, most of it moving list entries to keep them in
id order (an optimised build).

### Observing joins and leaves

A system that must react as another kind of entity joins or leaves (set
something up for a new wheel, release it as it goes) observes it where its
components are registered:

```cpp
class RigSystem : public sfs::System
{
protected:
  void create() override
  {
    registerComponent<sfs::VehicleComponent>(); // getEntities(), onEntityAdded()
    observe<Wheel, sfs::TransformComponent>(
        [this](const sfs::Entity& wheel) { setUpWheel(wheel); },
        [this](const sfs::Entity& wheel) { releaseWheel(wheel); });
  }
  void update(double dt) override
  {
    for (const sfs::Entity& wheel :
         registry->query<Wheel, sfs::TransformComponent>())
      spin(wheel, dt);
  }
};
```

`observe()` takes the same terms as `query()`, filters included
(`observe<Wheel, sfs::Excluding<Hidden>>(...)`). The callbacks (the second is
optional) are called with the same timing as `onEntityAdded()` and
`onEntityRemoved()`; the list itself is read with `query()`. For one change,
every list is updated first, then the systems are told in the order they were
added and, within a system, its `onEntityAdded()`/`onEntityRemoved()` first
and then its observe callbacks in the order they were declared. Observe only
where a reaction is needed: to just read a list, query it.

A program can have at most `MAX_COMPONENTS` (128) component types; one more is
a fatal error when it's first used.

`setEnabled(false)` skips a system's update/render (handy for debug toggles).

### Late update

`lateUpdate(double dt)` is Unity's `LateUpdate`: a `Scene` calls it once a
frame on every enabled system, in the order they were added, after every
game object's `onUpdate()` and the scene's own `onUpdate()`. Use it for what
must follow what the objects did this frame, such as `CameraSystem` keeping a
camera that a script has just moved clear of walls. A `Real` system gets the
frame's real time; `Stepped` and `Frame` systems get the frame's game time,
in one step.

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

### One-per-scene components

A component a scene has exactly one of (a world clock, the weather) is read
with `registry->singleton<T>()` (scripts: `scene().singleton<T>()`): the
component, or null when no entity has one. It costs nothing to call every
frame: the registry keeps track of who holds each component type as
components are added and removed. If several entities have one, it logs a
warning once and returns one of them. Use it instead of a query of one.
Unlike a query, it includes an entity still waiting for its first flush.

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
reg.flushEntities();             // new entities join queries and systems

reg.destroyEntity(e);
reg.flushEntities();             // destruction is deferred until flush
```

Entity destruction is deferred: a destroyed entity stays alive (and its
components remain readable) until the next `flushEntities()` call. New
entities join queries and systems at the flush too, so call `flushEntities()`
whenever the caller needs a consistent snapshot.
