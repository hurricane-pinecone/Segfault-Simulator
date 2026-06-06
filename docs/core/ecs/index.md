# Entities, components & systems

The core of SegFaultSimulator is a small **entity-component-system** (ECS), with an
optional Unity-style **GameObject** layer on top. You'll use both: ECS for
data-driven behaviour over many entities, GameObjects for individually scripted
actors.

![The ECS: entities carry components; systems run over the entities that match a component signature](../../images/ecs.png)

## The model

- **Entity** — a lightweight id. It holds no data itself; you attach components.
- **Component** — a plain data struct (position, sprite, health, …). No behaviour.
- **System** — behaviour that runs each frame over every entity carrying a chosen
  set of components.

You create entities and systems through the **`Scene`** (your level/screen), and
mutate components through the **`Entity`** handle. The engine's `Registry` owns the
storage, but game code never touches it directly — only a `System` subclass does,
through its protected `registry` pointer.

## Components

A component is any struct — define your own:

```cpp
struct Health   { int current = 100; int max = 100; };
struct Velocity { glm::vec2 value{0.0f, 0.0f}; };
```

The engine also ships components such as `TransformComponent` (position / scale /
rotation), `SpriteComponent`, and `ParticleEmitterComponent`. Up to 128 distinct
component types can be registered across the engine and your game (`MAX_COMPONENTS`).

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

## Systems

A system declares which components it cares about and runs logic over the matching
entities each frame. Subclass `System`, register its components in `create()`, and
do the work in `update()`:

```cpp
class MovementSystem : public sfs::System
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
      t.position += v.value * static_cast<float>(dt);
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
scene.addSystem<MovementSystem>();
auto& move = scene.getSystem<MovementSystem>();
if (scene.hasSystem<MovementSystem>()) { /* ... */ }
```

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

Rule of thumb: use **systems** for behaviour over many homogeneous entities
(movement, physics, rendering); use **GameObjects** for individually scripted
actors. Both sit on the same entities, so they compose freely.
