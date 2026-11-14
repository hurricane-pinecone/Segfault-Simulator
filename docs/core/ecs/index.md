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

A component describes the entity it's on, and doesn't store that entity: the
systems working on it are given it. A component that holds its own entity
(`state.entity = entity`) is flagged by [sfs-lsp](../../editor/sfs-lsp/index.md)
as `own-entity`.

The engine also ships components such as `TransformComponent` (position / scale /
rotation), `SpriteComponent`, `ParticleEmitterComponent`, and `NameComponent` (an
optional display name, useful for lookups and for showing entities in a tool's
hierarchy view). Up to 256 distinct component types can be registered across the
engine and your game (`MAX_COMPONENTS`). A development build logs a warning as
the ids come within 8 of that, so there's time to raise it.

## Entities

Spawn through the scene, then attach components via the returned `Entity`
(`addComponent` returns the entity, so calls chain):

```cpp
sfs::Entity e = scene.createEntity();
e.addComponent<sfs::TransformComponent>(glm::vec2{4.0f, 2.0f});
e.addComponent<Health>();
e.addComponent<Velocity>();

if (Health* health = e.getComponent<Health>())
  health->current -= 10;

e.removeComponent<Velocity>();
scene.destroyEntity(e);
```

`addTag<T>()` is `addComponent<T>()` read more naturally for empty marker
components — e.g. `e.addTag<Player>()`.

`getComponent<T>()` gives a pointer to the entity's `T`, or null when it has
none (or the handle isn't valid), in one inline lookup: test it before you use
it. `hasComponent<T>()` is for when you only ask (`if (e.hasComponent<Enemy>())`);
a `hasComponent` followed by a `getComponent` looks the component up twice,
and [sfs-lsp](../../editor/sfs-lsp/index.md) flags it. The pointer stays good
while other entities get components (each type's storage grows in pages that
never move), until that component is removed or its entity destroyed.
`removeComponent` destroys the component, and the entity leaves (or, adding
one, joins) the systems that no longer (or now) match it.

`matches<Terms...>()` asks the same of a whole query: whether the entity's
components match its terms, read exactly as a [query](#queries-lists-of-entities-by-component)
reads them (`Any<...>`, `Excluding<...>` and named `sfs::Query<...>` lists
included), as DOTS' `EntityQuery.Matches` does. It tests the components it
has now, as `hasComponent` does, so an entity not yet flushed into the
query's list already matches, and a destroyed one still matches until its
flush removes it. It's one signature test with no allocation, and false for a
handle that isn't valid:

```cpp
using Steered = sfs::Query<sfs::TransformComponent, Velocity, sfs::Excluding<Hidden>>;
if (e.matches<Steered>())
  steer(e);
```

An `Entity` is a handle: an id and a generation (24 bytes with its debug id
and registry). `isValid()` and `if (e)` tell whether it refers to a living
entity: false for a default-made handle, and once its entity is destroyed (at
the next flush), even after a new entity reuses the id. Then `getComponent`
gives null and `hasComponent` false, so a stale handle never reads another
entity's data. The check is one inline load and compare: a free id's
generation carries a dead bit that no handle has. A handle must not outlive
its registry, as `isValid()` reads it. Adding a component to a handle that
isn't valid logs an error naming the action, the type and why
("addComponent<Health>: entity 5 was destroyed; nothing added") and adds
nothing.

Where the code needs a component the scene's data should always give it (a
prefab's own parts, a relationship's other side), `sfs::expect` checks the
lookup in one line, as Unreal's `ensure` does: it returns false when the
component is missing, so the code takes its fallback instead of crashing:

```cpp
Hitch* hitch = vehicle.getComponent<Hitch>();
if (!sfs::expect(hitch, vehicle))
  return;
```

It's loud while you make the game and quiet in a shipped one:

- The first failure at each call site in a run logs an error naming the call
  site, the entity and the type ("HitchSystem.cpp:122: entity 812 has no
  Hitch"). Later failures there say nothing more.
- In development and editor builds (the debug guard, `debugFeatures`), that
  first failure also breaks into an attached debugger, and shows the engine's
  error dialog (the one a fatal error uses). Dismiss it and the game carries
  on with the fallback. A run that sets `SFS_NO_ERROR_DIALOG` (an automated
  test) shows no dialog: it stays a log line. Set `SFS_EXPECT_ABORT=1` to
  abort on it instead.
- In a release build it only logs, once per call site, and never crashes.

A passing check is one branch, and the message is built only on failure.

A lookup needs no check where it can't miss: a query row's components (see
[Rows](#rows-unpacking-the-components)), a component just added to a living
entity, and an observed component in `onEntityAdded()` or an observe added
callback.

In any hook or callback run while a component is being removed (every one a
`removeComponent<T>()` sets off: the leaving lists' `onEntityRemoved()` and
removed callbacks, and the `onEntityAdded()` and added callbacks of lists
with an `Excluding<T>` that the entity now joins, with whatever they call),
`getComponent<T>()` still gives the component (it's destroyed after them)
while `hasComponent<T>()` and `find<T>()` say it's gone, as EnTT's
`on_destroy` and Unity's `OnDestroy` let a removal observer read what's
going.

Engine code walking ids it knows are alive (the hierarchy's links, its
preorder index) can skip the handle check: `registry.find<T>(id)` gives the
component, or null when the entity has none, in one lookup (agreeing with
`hasComponent`, removal hooks included). Debug builds still check the id is
alive (logging an error and giving null when it isn't); release builds don't.
A loop over a query reads its components from the rows instead.

`scene.entities()` returns every living entity, in id order (`registry.entities()`
in headless use). This is handy for a tool that needs to list every entity
regardless of which components it carries.

The registry also keeps the parent/child hierarchy, beside its other per-id
arrays: 16 bytes of links per entity (parent, first child, next and previous
sibling), changed by `hierarchy::setParent` and by the flush that removes an
entity. There's no component for it. `scene.preorder()` lists every entity
parents first, the way a hierarchy panel shows them. See
[Parents & children](../hierarchy/index.md).

## Systems

A system declares which components it cares about and runs logic over the matching
entities each frame. Subclass `System`, register its components in `create()`, and
do the work in `update()`:

```cpp
class DriftSystem : public sfs::System
{
  using Entities = sfs::Query<sfs::TransformComponent, Velocity>;

protected:
  void create() override { registerComponent<Entities>(); }

  void update(double dt) override
  {
    // The entities with BOTH components, each row unpacking them (add the
    // entity first, `auto [entity, transform, velocity]`, to use it too).
    for (auto [transform, velocity] : getEntities<Entities>().components())
      transform.setPosition2D(transform.position2D() +
                              velocity.value * static_cast<float>(dt));
  }
};
```

See [A system's entities](#a-systems-entities-registercomponent) for what
`getEntities()` holds, and [Queries](#queries-lists-of-entities-by-component) for
lists of other kinds of entity.

A system has four optional lifecycle hooks: `create()` (once, when it's added
to a registry, with its registry set: it registers components and observers,
sets its time step and builds what it builds once; the constructor only
initialises members), `update(double dt)` (per frame), `lateUpdate(double dt)` (per
frame, after the game objects; see [Late update](#late-update)) and
`render()` (per frame, after update). Add it to a scene and fetch it later:

```cpp
scene.addSystem<DriftSystem>();

DriftSystem& drift = scene.getSystem<DriftSystem>(); // system must be present
if (scene.hasSystem<DriftSystem>()) { /* ... */ }

// When presence is conditional, use tryGetSystem: it returns null if absent.
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

`registerComponent<T>()`, called in the system's `create()`, adds `T` to the
components an entity needs to be one of the system's entities (one called
after the system was added is logged and ignored). `T` may also be a filter group, as a query takes them (see
[Any and Excluding](#any-and-excluding)):
`registerComponent<sfs::Excluding<Hidden>>()` leaves out hidden entities,
`registerComponent<sfs::Any<A, B>>()` asks for one of a group (one `Any` group
per system: a second is logged and ignored), and
`registerComponent<sfs::Query<...>>()` registers each term of a named list
(see [Naming a long query](#naming-a-long-query-query)). `getEntities()`
returns them: the living entities that match its registered terms, in id
order. It is a view of the registry's shared query for those terms (see
[Queries](#queries-lists-of-entities-by-component)), so the system keeps no
copy, and it is read-only.

`getEntities()` goes through entities. With terms, its rows unpack them
(see [Rows](#rows-unpacking-the-components)): a system with more than a
couple of terms names them once, `using Entities = sfs::Query<...>;`,
registers `registerComponent<Entities>()` and goes through
`getEntities<Entities>()`, as `DriftSystem` above does; a loop that needs fewer
asks for them by name (`getEntities<Velocity>()`). The terms asked for must be
implied by the registered ones (every entity the system lists matches them,
checked once per call with `QuerySignature::implies`, a few 256-bit
operations), so a row's component is always there: terms the system can't
vouch for log an error, once per list of terms, and give an empty view.

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
system's own state and that entity: a hook may add a component to the entity
joining, or remove one from an entity leaving because it lost a component (not
from one being destroyed: it still has them all, and the flush removes them).
Such a change updates the lists naming that component then and there, as
`TransformSystem` does to give each entity with a transform its world cache.
Don't add or remove other entities' components, entities or systems from
them.

### Queries: lists of entities by component

`registry->query<A, B>()` (scripts: `scene().query<A, B>()`) is the list of
entities with both components, in id order: the living ones that have been
flushed, with the same timing as a system's entities. The registry makes it
the first time it's asked for (one scan) and keeps it up to date from then on
as components are added and removed, so reading it every frame costs nothing,
and everyone asking for the same components shares the list. The list stays
valid as long as the registry.

```cpp
for (auto [transform, point] :
     registry->query<sfs::TransformComponent, sfs::PointLightComponent>()
         .components())
  draw(transform, point);
```

Use a query for any other kind of entity a system or script reads; a system's
own kind is `getEntities()`.

#### Rows: unpacking the components

`query()` returns a `QueryView`, the shared list read in place (two
pointers, never a copy). Going through it gives rows: each converts to its
entity, so `for (const sfs::Entity& e : registry->query<A>())` goes through
the entities alone, and unpacks as `[entity, components...]`:

| Term | Binds |
| --- | --- |
| a component `T` | `T&`, read straight from its slot: every entity in the list has one; `const T` gives `const T&` |
| `sfs::Any<A, B>` | `A*`, `B*`, null when the entity has none |
| `sfs::Excluding<...>` | nothing |
| `sfs::Query<...>` | its terms, in order |

`query<Position, sfs::Any<Velocity, Health>, sfs::Excluding<Hidden>>()`
unpacks as `[entity, position, velocity, health]`, the last two pointers.
Rows are values made at each step, so unpack them with `auto`, `const auto&`
or `auto&&`; `auto& [...]` doesn't compile. The bindings are references
whatever the qualifier: `const auto& [e, a]` still gives a writable `A&` (an
`Any` member is then `A* const`), as in EnTT. A loop over the entities alone
names the type: `const sfs::Entity&` or `sfs::Entity`. `for (auto e : view)`
gives a row, not an `Entity`, and `for (auto& e : view)` doesn't compile. A
row reads only the components it binds, and a binding the body never reads
costs nothing once optimised, so there's no need for a narrower query just to
skip one.

A component may be written `const`, in a term or a lookup, and is then the
same component, read-only: `query<const Position>()` is the same list as
`query<Position>()` and its rows bind `const Position&`, `Any<const A, B>`
binds `const A*`, and `getComponent<const Position>()` (and `find`, `finder`,
`singleton`) gives a `const Position*`. `const` goes on the components
themselves, inside an `Any`, `Excluding` or `Query` too: `const
sfs::Query<...>` doesn't compile, and neither does adding or removing a
`const` component.

A loop that doesn't need the entity leaves it out. A row that binds exactly
one component, a required one, converts to it, and `components()` gives the
same rows without their entity, unpacking as `[components...]`:

```cpp
for (Position& position : registry->query<Position, sfs::Excluding<Fixed>>())
  position.y = 0.0f;
for (auto [position, velocity] :
     registry->query<Position, Velocity>().components())
  position.x += velocity.dx * dt;
```

They read through the same finders, cost what an entity row does, and get the
same debug check that the list hasn't changed. Name the component by
reference: `Position&`, or `const Position&` to read it (the only form a
`const Position` term allows). `Position p` copies it, so changes to `p` are
lost; `auto p` is the row, not the component; `Position&&` doesn't compile.
`components()` rows unpack with `auto`, `const auto&` or `auto&&`, as entity
rows do. A row with an `Any` member never converts, since that binds a
pointer that may be null (a lone `Any<A>` lists what `A` does: write `A`);
use `components()`. The typed hierarchy walks (`childrenOf<...>()`,
`descendants<...>()`) give the same rows and have `components()` too.
`sfs-lsp` flags an `auto [e, ...]` loop whose entity is never used
(`unused-row-entity`) and rewrites it to these forms, and flags a loop that
copies its one component (`row-component-copy`), reading it in place when
the loop only reads it.

`size()`, `empty()`, `[i]`, `front()` and `back()` give entities.
`entities()` gives the registry's `const std::vector<sfs::Entity>&` itself,
and the view converts to it, so code that stores or passes the list
(`const std::vector<sfs::Entity>& hands = registry->query<FarmHand>();`)
takes the view as it is. `auto list = registry->query<...>()` holds a view,
not a copy; write `std::vector<sfs::Entity>(registry->query<...>())` for a
copy. Standard algorithms over the entities take `entities()`.

#### Changing components while going through a list

- **Safe in the loop:** changing any component's values; adding or removing
  components the list doesn't name; creating entities (they join at the next
  flush); destroying entities (they leave at the flush). Components bound from
  rows stay where they are while other entities gain or lose components.
- **Never:** add or remove a component the list names, on any entity, while
  looping the list. It changes the list under the loop (a sorted insert or
  erase, maybe a reallocation). Go through a copy. A copy doesn't unpack: by
  the time a copied entity is reached it may no longer match, so look its
  components up and check them.

Debug builds (the editor's play mode included) catch the misuse and **end
such a loop**: each list counts its inserts and erases, and a loop that finds
the count changed since it began logs an error naming the query and the
systems following its list ("a query list changed while a loop went through
it: ..."), then stops rather than read a moved list (as Unity DOTS does for a
structural change during iteration). Release builds pay nothing per row and
don't stop, so the same loop behaves differently there: a misuse is undefined.
Treat that error as a bug to fix, never as a way to end a loop.

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
  [sfs-lsp](../../editor/sfs-lsp/index.md#query-terms) flags the redundant
  terms in your code, and term lists that can never match.
- A loop that adds or removes a component its own query names changes the
  list it is going through (see
  [Changing components while going through a list](#changing-components-while-going-through-a-list)).
  Filters make this easy to write by accident: "mark everything not yet marked" is
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

#### Naming a long query: Query

`sfs::Query<...>` is a list of terms that stands for those terms written out:
the same signature, so the same shared list. Name a long one once, as a type
alias inside the system (its class or its `.cpp`'s anonymous namespace), and
use the name wherever the list is read, registered or observed:

```cpp
class TrafficSystem : public sfs::System
{
  // What drives itself: steered and moving, by a driver or an autopilot, and
  // not parked.
  using Driven = sfs::Query<sfs::TransformComponent, Steering, Velocity,
                            sfs::Any<Driver, Autopilot>, sfs::Excluding<Parked>>;

  void update(double dt) override
  {
    for (auto [transform, steering, velocity, driver, autopilot] :
         registry->query<Driven>().components())
      drive(transform, steering, velocity, dt);
  }
};
```

- It takes what a query takes: components, at most one `Any` group (counting
  those beside it: a second is a compile error) and `Excluding` groups.
- It mixes with other terms and with other `Query` lists, in any order:
  `query<Driven, Health>()` is the query with `Health` added, and
  `Query<Driven, sfs::Excluding<Hidden>>` names a narrower one in turn.
- `registerComponent<Driven>()` registers each of its terms, and
  `observe<Driven>(...)` observes the same list.
- It is flattened at compile time and costs nothing at run time. Keep short
  queries written out: a name earns its place only where the list is long or
  repeated.
- A list several systems share is named once, beside its component (or in a
  shared header), not again in each system.

#### What lists cost

The registry keeps each list's `sfs::QuerySignature` (three 256-bit sets: all,
any and excluding, 96 bytes) in one dense array, the lists themselves by the
same index, and, for each component type, the indices of the lists that name
it (4 bytes per list per component it names). It stores nothing per entity
beyond the lists. Reading a list is a hash lookup on the signature, which each
`query<...>()` builds once.

An entity gaining or losing a component is tested only against the lists that
name that component: two signature tests each, plus the sorted insert or
erase where it joins or leaves (and a 4-byte change counter bumped). Systems
are told only when a list one of them follows changed. A flush tests each
entity joining or leaving against every list, streaming the dense signatures.
`ecsBenchTests` times this with 120 query lists over 40 component types, 60
systems and 2000 entities:
a component no list names costs about 20 ns to add or remove, one a few lists
name about 0.5 µs, and one about half the lists name about 2.6 µs, most of it
moving list entries to keep them in id order (an optimised build).

Going through a list costs only what a row binds: a required component is one
read from its slot (membership is the list's), an `Any` member a bit test and
then the slot, and nothing is allocated. A `QueryView` is 16 bytes, a row two
pointers, and a loop's iterator one entity pointer plus one finder (24 bytes)
per bound component, made once where the loop begins. `ecsBenchTests` (Release
build, best of 200 rounds; 2,000 entities, 50,000 in brackets; ns per row or
lookup):

| Case | ns |
| --- | --- |
| entity-only loop over a view (the same as over the vector) | 0.33 (0.47) |
| rows binding 1 component, unpacked `[entity, a]` | 0.90 (1.2) |
| the same, by the row's conversion (`A& a`) | 0.90 (1.2) |
| rows binding 2 components | 1.2 (2.0) |
| the same through `components()` | 1.3 (2.0) |
| rows binding 3 components | 2.0 (4.3) |
| the same through `components()` | 2.1 (4.4) |
| a `getComponent` per component, 2 components | 5.9 (6.3) |
| a `Registry::Finder` per component, 2 components | 2.7 (3.4) |
| `getComponent`, a hit at a random entity | 3.3 (5.7) |
| `getComponent`, a miss at a random entity | 2.8 (5.1) |
| `hasComponent`, a miss at a random entity | 2.1 (3.8) |
| `isValid()` at a random handle, a quarter stale | 1.1 (1.8) |

Rows are two and a half to five times faster than a `getComponent` per
component in the loop (4.9 times with 2 components at 2,000 entities, 2.6
times with 3 at 50,000), and faster than finders. A `getComponent` miss costs
a little more than a `hasComponent` one (it reads the component's slot rather
than the entity's bit), so a test that only asks uses `hasComponent`; a
`hasComponent` then `getComponent` pair costs both, and one `getComponent` is
cheaper unless nearly every lookup misses.

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

A program can have at most `MAX_COMPONENTS` (256) component types; one more is
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
