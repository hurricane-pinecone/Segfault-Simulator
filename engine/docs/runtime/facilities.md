# Facilities internals

How the engine's facilities are built, and how to add one. What they are and
how game code uses them: [Engine facilities](../../../docs/runtime/facilities/index.md).

## The pieces

- `engine/include/engine/core/ecs/facilityAccess.h` (core) forward-declares
  the facility classes (`sfs::facility::Physics3D`, `Navigation`, ...) and
  `detail::HostAccess`.
- `System`, `Service` (protected), `GameObject` (protected) and `Scene`
  (public) each declare one `inline` accessor per facility, returning it by
  value, with no body.
- `engine/include/engine/runtime/facilities/<name>.h` defines the facility
  class and the four accessors' bodies, inline. Including it gives both, and
  a call needs the complete return type, so the include can't be forgotten:
  without it, a call is an incomplete-type error. Core never calls an
  accessor, so it still builds and links alone.
- `engine/src/runtime/facilities/<name>.cpp` has the bodies of the methods,
  out of line, so a facility header never includes an engine system's header:
  the facility header is the game's whole view. Types a facility's methods
  need that lived in a system header moved to their own (`RaycastHit3D`,
  `RenderStats`).
- `absentBackend.h/.cpp` (in `src/`): `warnAbsent()`, the once-a-run warning.

## Shape and cost

- A facility is a value handle: one pointer to what backs it (`Cameras`,
  `DebugDraw` and `Meshes` two: `Meshes` holds the RenderSystem and the
  registry its picks walk), private, never handed out. It never returns a System or
  the registry. All its methods are `const`, as a handle's are.
- `Facility::in(Registry*)` makes one. System-backed facilities find
  their system with `Registry::tryGetSystem<T>()` (one atomic load from the
  registry's 256-slot lookup cache, plus a type check in development builds).
  `Assets::in()` reads the scene's `AssetStore`, which `Scene`'s constructor
  gives its registry as a private host pointer (`Registry::m_assetStore`),
  read only through `detail::HostAccess::assetStore()`.
- The accessors: a System's passes `registry`, a Service's its private
  `m_registry`, a Scene's `&m_registry` (so a Scene's accessors aren't
  `const`, as its `query()` isn't), and a GameObject's asks its scene's.
- Every in-header body (`available()`, the private constructors,
  `DebugDraw`'s drawing, `Graphics`' templates) is marked `inline`, and each
  header includes `system.h`, `service.h`, `gameObject.h` and `scene.h` for
  the accessors it defines.
- Each method is one direct call to the engine method it forwards plus a null
  check, returning what the engine returns, by the same reference, span or
  shared pointer: nothing is copied. No allocation, no virtual call.
- `DebugDraw`'s drawing methods are inline on the frame's `DebugLines`, under
  `if constexpr (debugFeatures::compiled())`, so a release export compiles
  them away. `DebugDraw::in()` gives a null handle while
  `debugFeatures::enabled()` is false.
- Memory: one `static std::atomic<bool>` per backend (two for `Cameras`),
  for its warning: once per process, loaded before it's exchanged.

## Absent backends

A facility made in a scene without its backend holds null. Its private
`backend()` returns the pointer, calling `warnAbsent()` first when it's null:
the first use in a run logs one warning naming the missing part, and later
uses log nothing. The message names the missing part ("used where there's
no PhysicsSystem3D"; `Assets` says "a registry with no scene"). An engine
system listed after the caller doesn't exist yet in the caller's `create()`,
so a facility used there is absent: the docs say to set up through it in the
first update instead. Queries then return an empty answer (`nullopt`, null,
`false`, an invalid entity, zero, a `static const` default for a reference
such as `Navigation::settings()`), commands return. `available()` reads the
pointer without warning. `DebugDraw` never warns.

## The rules it keeps

- **Game code never gets a system.** The facilities wrap the engine systems
  privately. `getSystem`/`tryGetSystem`/`hasSystem`/`forEachSystem` stay public
  for hosts, the editor and tests until the refactor's end (E-ENGINE-CALLS-2),
  when they move to `sfs::detail::SystemAccess` and a lookup in game code
  stops compiling; the `system-lookup` lint flags them in projects meanwhile.
  The facility `.cpp` files will then use `SystemAccess`; nothing else
  changes.
- **Services read, never change, what a facility finds.** A Service may use
  the facilities in its handlers. A facility hands out answers, never lists,
  so the Service wall holds: an entity a query returns (a raycast's hit) and
  what it leads to (its parent, its parts) the service may read, but it
  changes only the entities its events hand it and those it makes itself
  (`createEntity()`, `prefabs().spawn()`). An `sfs::Entity` has no read-only
  form to hand out cheaply, so this is documented (docs/core/services,
  docs/runtime/facilities), not enforced.
- **The engine's own systems use the facilities too**: NavigationSystem takes
  its solids from `physics()` and draws with `debugDraw()`.
- **State on a component is written there.** A facility has no setter for
  what the engine reads from a component (`NavAgentComponent::hitch`,
  `speedCap`, `active`; `AnimatorComponent`'s steady state). NavigationSystem
  copies every agent's `speedCap` into its fleet at each sync: its update, and
  every order that plans (`setPath`, `requestGoals` through `syncAgents()`,
  which ends in `syncSpeedCaps()`; `planAhead`, `planNow` through
  `syncSpeedCaps()` alone). A read of `solid()` between a cap's write and the
  next sync sees the old cap.
- **One-off orders are calls.** `animation().play(clip, fade)` (a cross-fade
  of its own) and `restart()` (fading clips dropped) are immediate commands
  per D1, as `requestGoal()` is. They leave their note on the
  AnimatorComponent's hidden `fadeRequest` and `restart` fields, which
  AnimationSystem takes up at its update: engine-internal state no game
  writes. Moving them into the system would need per-entity storage in it,
  against the component rule, so they stay.
- **Lifetime.** A facility is valid for the step that got it; `keeps-facility`
  flags one kept in a member. What a worker needs is a `shared_ptr` the
  facility hands out (a physics snapshot, the planning scheduler, a lane or
  coarse graph).

## Adding one

1. Forward-declare the class in `facilityAccess.h`.
2. Declare its accessor, `inline facility::X x() const;`, in `System` and
   `Service` (protected), `GameObject` (protected) and `Scene` (public), in
   alphabetical place, with a one-line docstring.
3. Write `runtime/facilities/x.h`: the class (its static `in()`, its methods,
   `available()`, a private constructor and `backend()`), then the four
   accessor bodies. Include only what its signatures need, never the system's
   header: move a type a method needs out of a system header if it's there.
4. Write `src/runtime/facilities/x.cpp`: `in()`, `backend()` with its
   `s_warned` flag, and one forward per method with its empty answer.
5. Add the header to `<sfs/sfs.h>`, a test in
   `engine/tests/runtime/facilities/facilitiesTests.cpp` (available from all
   four places, empty answers and one warning when absent), and its row in the
   game-dev page's table.

A new need on an existing facility adds one forward, not a lookup. Not
exposed, on purpose: engine and editor knobs (visual styles, the sky, the
editor's animation preview, `setLive`), test getters (`lastUpdateStats`,
scene-wide contact lists: games read `ContactReportComponent`) and
`cameraFeed()` (a UI reads `CameraFeedComponent::picture`).
