# Segfault Simulator (SFS): rules for AI agents

SFS is a general game engine. Nothing game-specific goes in it. Read README.md and docs/ before changing an area.
Engine bar: a feature belongs in SFS only if a production-ready general engine (Unity, Unreal, Godot) would ship it
(in the engine itself, or as a first-party library once SFS has them). Anything narrower is game code, even if more
than one game could use it.
These rules are the owner's; when a task seems to need breaking one, stop and ask.
Consult on structure, decide within it: the approved architecture is the design. Within it, split responsibilities
the best-practice way without asking, and split properly from the start. Ask the owner only when a change would alter
the architecture or these rules, or how a game behaves for the player.
**Blazingly fast, by design.** SFS is built for speed at the memory level from the start, not optimised later. Data
lives in one place and is read where it lives: no copies, no parallel or mirrored structures, no per-frame
allocation (reuse buffers, pools and arenas). Hot data is contiguous and cache-friendly (dense arrays of plain data,
struct-of-arrays where systems stream one field, no pointer chasing or node-based containers on hot paths). Work is
incremental (dirty flags, change versions: only what changed is recomputed) and parallel where it's independent.
Every new engine design states its memory layout, what it allocates and when, and its per-frame cost; reviews check
it. The standard engine approach is the baseline; do better where the data allows.

## Architecture

### Layers
- `engine-core` is the ECS (entities, components, systems, reflection, scripting, assets core) and has ZERO
  dependencies. Components (plain data) live in `engine/include/engine/core/components/`.
- Features go in `engine-runtime` (`engine/include/engine/runtime/...`, `engine/src/runtime/...`). A feature is an
  ECS **System** in `runtime/systems/` (pattern: `PhysicsSystem3D`) plus data-only components. Its internals go in a
  folder named for the feature (e.g. `runtime/navigation/`).
- "Modules" means RENDER modules only (`runtime/rendering/modules/`: MeshModule, GrassModule...). Never call a
  system or a library a module.
- `engine-webgpu` is the render backend. The editor (`editor/`) consumes the engine like any game does.

### Components
- Components hold ALL per-entity data, and NO logic: its state, including what changes in play (a transform's
  position, a machine's speed or fuel), and anything a system works out or keeps about that entity (a plan or
  search in progress for it, a cache derived from it, a handle to its engine-side job, a logged-once flag). Systems
  hold the logic and update it. A system never keeps per-entity data of its own: no map or struct keyed by entity.
- A system's own data is only what isn't about any one entity it iterates: shared caches and indexes, scratch
  buffers, handles to engine internals, settings, debug views. Iterate `getEntities()`; use `onEntityAdded()` /
  `onEntityRemoved()` to set up or release what an entity joining or leaving needs, never to keep a parallel copy.
- The data's types belong to the components (their headers, or plain data headers), never to a system: no
  system-defined struct held from a component by pointer.
- Systems are single-purpose and never read other systems: what a system needs from another concern comes through
  components.
- How systems communicate: a system reads and writes only components. It hands work on by writing a component a
  later system reads, in the registered update order. A request is a component field the serving system fills in
  later (a plan request and its result). When two systems feed each other, the earlier one reads what the later one
  wrote last frame. No `getSystem` for data, and no callbacks between systems.
- Lists of entities by component: a system's own are `getEntities()` (its `registerComponent`s); any other kind is
  `registry->query<Ts...>()` (scripts: `scene().query<Ts...>()`: cached, kept current, shared). A system that must
  react as another kind joins or leaves uses `observe<Ts...>(added, removed)`. No system keeps its own entity list.
- A system's set-up goes in `create()` (called once, when it's added, with its registry set): registering components,
  observers, time step, and anything it builds once. The constructor only initialises members.
- A component a scene has exactly one of (a world clock, the weather) is read with `registry->singleton<T>()`
  (scripts: `scene().singleton<T>()`), never a query of one.
- Component fields the editor can't edit (or that shouldn't be saved or shown) are `[[sfs::hide]]`.
- A component describes its own concern only. It never holds references to another feature's data or assets: e.g.
  `NavAgentComponent` does not reference a `VehicleSpec`. A system that needs several concerns views the entities
  that have all of those components. A component may reference an asset that IS its own concern (MeshRenderer ->
  mesh, VehicleComponent -> VehicleSpec).
- One concern per component: a component holds one concern's data. Add a component for a new concern rather than
  adding its fields to an existing one. Shared per-model data goes in a ScriptableObject asset, not a component.
- Components are never extended by inheritance (ECS lookups are by exact type).

### Data and assets
- Data shared by many entities (a vehicle model's specs, a crop type) is a ScriptableObject (`.asset`), referenced
  with `sfs::AssetRef<T>`. Games may extend engine ScriptableObject types (`TractorSpec : sfs::VehicleSpec`).
- Store PROPERTIES, never physics OUTCOMES. Power, mass, brake force, wheelbase and tyre sizes are properties.
  Acceleration, deceleration, grip, flotation and cornering limits are outcomes: derive them from the properties
  plus load and surface. A deliberate behaviour cap is a clearly named behaviour setting, not a physics field.
- ONE source for physics. Base values come from one defined place (spec assets, tyres, model nodes and colliders).
  Each derived value (accel, braking, grip, turning radius, cusp time, driven speed, stopping distance, collision
  shapes and poses) has one function that computes it (separate functions are fine). The planner and the driver
  (speed profile, follower, look-ahead, traffic, game systems) call the SAME functions: there is no separate
  planner physics. No local copies, constants or fudge factors (e.g. "brake * 2") anywhere else.

### Extensibility
- Pluggable implementations are registered by name through a factory/registry (pattern: `RenderBackends::add`).
  Examples: navigation agent kinds ("vehicle", later "walker") and path searches ("hybridAStar").
- Build one implementation and one interface. No speculative abstraction beyond that.
- Reuse first. Before writing code, search the engine and game for code that already does it and reuse or extend it.
  Never add a second implementation, copy or parallel path for the same concern (e.g. planner vs driver, game vs
  engine). Fix a mismatch by deleting the duplicate, not by syncing numbers.
- Standard problems get the textbook solution (e.g. vehicle travel: coarse graph -> hybrid A* -> curvature-
  continuous path -> speed profile -> path follower). Bespoke logic belongs in the game.

### Debug features
- Everything debug (debug drawing, debug env vars, runtime toggles) goes through `debugFeatures.h`. It works in the
  editor, Debug builds and development exports only, and is compiled out or forced off in release exports.
- Draw with `RenderSystem::debugLines()`. Colliders: Scene Settings > Debug > Draw Colliders / `SFS_DRAW_COLLIDERS`.

### Collision
- Collision uses engine colliders (BoxCollider3D etc., Jolt). There are no hand-rolled footprints.
- Colliders are authored data in scenes and prefabs (auto-fit on add, Fit to Mesh, `*-collider` model nodes). Code
  never generates them at runtime.
- Simple shapes are fine: tiny clipping is acceptable. Mesh colliders are a possible later feature.

### Errors
- Never throw fatal exceptions, and never abort or crash on bad input or state. Log it with `LOG_ERROR` /
  `LOG_WARN` (`engine/core/logger/logger.h`; logs will also be written to a file), then carry on in a safe fallback:
  skip the item, use a default, or return an empty/failed result the caller can check.
- Don't let exceptions from libraries escape into the game loop: catch them at the boundary and log them.
- No error popups in test runs (`SFS_NO_ERROR_DIALOG=1`).

## Reflection and the editor
- Codegen (`engine/tools/codegen`, tree-sitter) finds types by base class. There are no registration macros.
  Attributes: `[[sfs::hide]]`, `[[sfs::show]]`, `[[sfs::showIf]]`, `[[sfs::range(a,b)]]`, `[[sfs::color]]`, etc.
  See `parser.cpp parseAttributes()`. Never use comment annotations (`sfs-ignore` directives aside).
- Inspector rows put the label on the left and the widget on the right (`widgets::propertyLabel`, `propertyCheckbox`).
  Hide settings that don't apply.

## Workflow
- The OWNER commits. Never commit, stash, reset, revert or push.
- Every engine feature gets engine unit tests (`engine/tests/...`) and a docs page (`docs/`, `mkdocs.yml`). Run only
  the tests your change touches.
- Builds happen in WSL. Windows is cross-built with MinGW into `build-win/`. Use `nice -n 15 cmake --build ... -j3`
  (wider builds freeze WSL). Only one build at a time.
- clang-format 17.0.6, on `.cpp`/`.h` files only (never md, yml, wgsl or CMake). Some files in HEAD aren't
  formatted: don't reformat code you didn't touch.
- Screenshots: only the game's or editor's own windows, never the desktop.
- Comments: plain, human-readable sentences, and only where the logic is complicated or not obvious. Don't narrate
  what the code does, keep history, or add parenthetical asides. Every public and util function gets a proper
  docstring (`/** ... */`: what it does, its parameters, its result). Fields other code reads (a component's fields
  read by systems, a shared struct's) each get a docstring. Applies to all code written or changed.
- Optimise as you go when it's simple and doesn't change behaviour: take by const reference instead of copying
  (maps, vectors, strings, components), move instead of copy, reserve when the size is known, hoist repeated lookups
  out of loops. Leave larger optimisations for a measured change.
