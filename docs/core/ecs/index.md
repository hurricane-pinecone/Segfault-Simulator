# Entities, components & systems

The core of SegFaultSimulator is a small **entity-component-system** (ECS), with an
optional Unity-style **GameObject** layer on top. You'll use both: ECS for
data-driven behaviour over many entities, GameObjects for individually scripted
actors.

## The model

- **Entity** — a lightweight id. It holds no data itself; you attach components.
- **Component** — a plain data struct (position, sprite, health, …). No behaviour.
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
catches it, tells the player in a message box and ends the game (set the
`SFS_NO_ERROR_DIALOG` environment variable to skip the message box, for an
automated run with nobody there to close it: the log gets the message instead,
and the game still ends). Use `tryGetSystem<T>()` when a system may or may
not be present.

`scene.view<A, B>()` makes a list; `registry->each<A, B>(fn)` (in a system)
calls `fn(entity, a, b)` for each match without one, for code that runs every
frame.

A program can have at most `MAX_COMPONENTS` (128) component types; one more is
a fatal error when it's first used.

`setEnabled(false)` skips a system's update/render (handy for debug toggles).

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

In a `StandardScene` (a scene file played), `scene.instantiate("asset:<id>")`
makes a prefab's instance while the game runs and starts its scripts, as when
the scene started.

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
