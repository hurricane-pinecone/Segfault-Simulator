# Services internals

How services work inside the engine. What they're for and how a game writes
one: [Services](../../../../docs/core/services/index.md).

The code is `engine/include/engine/core/ecs/service.h`, `service.inl` (its
template bodies that need the complete Registry),
`engine/src/core/ecs/service.cpp`, and the hosting in `registry.h`,
`registry.inl` and `registry.cpp`.

## The wall

A service may touch only what its events hand it. Everything it may not do is
a compile error, not a run-time check:

- `sfs::Service` derives from `EventVerbs`, as `System` does, so it shares
  `send()`, `send(key, e)`, `cancel<E>(key)`, `broadcast<E>()`,
  `requester<R>()`, `hasUnconsumedEvents()` and `enabled()`/`setEnabled()`.
- `Service::consumer<E>()` hides the base's and returns an
  `EventConsumerBuilder<E>` (`eventConsumer.h`). Its handler steps
  (`onEvent()`, `onEvents()`, `onLatest()`, `onLatestEvents()`) give an
  `EventConsumerBuilder<E, true>`, and only that one converts to
  `EventConsumer<E>`: the conversion's `static_assert(kHandled)` is the error
  for a consumer with no handler.
- What a System has and a Service doesn't (`query`, `getEntities`,
  `registerComponent`, `observe`, the `registry` pointer) is System's own, not
  EventVerbs', so in a Service those names aren't declared: a use fails to
  compile as any undeclared name does. The Services docs and the
  `service-reaches-registry` lint say why.
- The real registry pointer, `m_registry`, is private; Registry is a friend
  and sets it in `addService()`. The scene data calls (`sceneData<T>()`,
  `addSceneData<T>()`, `removeSceneData<T>()`), `createEntity()` and
  `destroyEntity()` are its only users. The scene data templates are defined
  in `service.inl`, which `registry.h` includes at its end (as it does
  `entity.inl`), where Registry is complete.
- `update`, `lateUpdate`, `render` and the hooks aren't virtual in Service,
  so `override` fails; one without `override` is dead code, which the
  `service-update-method` lint flags.
- `create()` is pure virtual: `addService<T>()` of a service without one
  fails to compile ("abstract").
- `addService<T>()` static_asserts that T derives from Service.

A bypass that names the base on purpose (`EventVerbs::consumer<E>()`) is not
guarded: it's contrived, and such a consumer only holds its queue, which the
65,536 warning names.

## No path from an entity to its registry

`Entity::registryOf()` and `setRegistry()` are private, so no code outside
the engine's internals reaches a registry through a handle. Registry is a
friend (it makes handles), and `detail::EntityAccess::registry(entity)`
(`entity.h`) is the engine's own way in, used by the hierarchy's walks
(`hierarchy.h`'s typed `childrenOf<...>()` and `descendants<...>()`, and
`hierarchy.cpp` through a file-local `registryOf()`) and by the scene
serializer's scene id scan. C++ has no package-private, so `detail` is a
convention, kept by the `engine-internal` lint in projects.

Reflection-driven tools find a type's entities with
`Scene::holdersOf(const reflect::TypeInfo&)`: the type's
`component.holders(registry)` (its query list), or an empty list for a type
that isn't a component.

## Hosting

The registry keeps its services in `std::vector<std::unique_ptr<Service>>
m_services`, in the order added.

- `addService<T>(args...)`: makes it, sets `m_registry`, attaches the events
  (`EventHost::attach()`), appends it, then runs `create()` between
  `EventHost::declaring(true)` and `(false)`, so handles made anywhere else
  are refused and logged.
- `runServices()`: a Tracy `services` zone; for each service, skipped if
  disabled or `!hasUnconsumedEvents()`; otherwise a `service/<Type>`
  cpuTiming scope around `begin()`, `dispatch()`, `end()` on its
  `EventOwner`, the same calls `updateSystem()` makes, with no update between.
- `Scene::update()` calls `runServices()` at the start of every step, after
  `advanceGameTime()` and before the systems' updates.
- `~Registry()` takes the services out of the list and destroys them first,
  in the order added, then the systems; the `Events` member goes after the
  body. A service's scheduler is joined while the systems and the events
  live, and its handles deregister from live queues.

## Waking

A service runs only when `EventOwner::hasUnconsumed()` finds a consumer whose
queue holds events past its place: per handle, the slot's generation and
`next < endId`. A requester's response consumers wake for any response of
their type (any requester's), as a system's do: a few loads to find its own
route empty. There are no wake lists (they would be written on send, the
hottest event path) and no Events-wide "sent anything" counter.

## Script registry and codegen

`sfs-codegen` classifies a class deriving `sfs::Service` (or another service)
as `TypeKind::Service` and writes a `ScriptType` with `Kind::Service` and
`addService`, beside the reflected `TypeInfo`. `startScripts()` adds a scene
file's `"services"` by name after its systems; `removeModule()` drops a
module's services with its systems.

## Layout and cost

| What | Layout | Allocates |
| --- | --- | --- |
| `Service` | vptr, `EventVerbs` (events pointer, its `EventOwner`, flags), the private registry pointer | the object at `addService()`; its handles' slots and handlers in `create()` |
| Registry | one vector of `unique_ptr<Service>` | a slot per service, at `addService()` |
| `EventConsumerBuilder<E>` | the `EventConsumer<E>` it becomes, moved, never copied | never |

Per step: nothing with no services (one empty loop); per idle service its
enabled flag and a compare per handle; a woken service costs a system's
handler dispatch without an `update()` call. Nothing is allocated per step
once warm.

## Benchmarks

`engine/tests/core/ecs/serviceBenchTests.cpp` (Release; gates checked under
`SFS_BENCH_GATES`):

| Case | Gate |
| --- | --- |
| `runServices()`, 16 services with 4 handles each, nothing new | at most 4 ns a handle |
| a woken service handling 1,000 news events with `onEvent()` | at most the same handler through a System's `updateSystem()` + 5 % (or + 50 ns) |
| a task swap per event (remove one component, add another; a system following both lists) | at most 1.1x the same swap in a System's update |
| 1,000 steps of steady traffic through 4 services after warm-up | no allocation (counted in a static build) |
