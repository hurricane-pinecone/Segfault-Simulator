# Segfault Simulator (SFS): rules for AI agents

SFS is a general game engine. Nothing game-specific goes in it. Read README.md and docs/ before changing an area.
These rules are the owner's; when a task seems to need breaking one, stop and ask.

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
- Components are DATA ONLY: no logic, and no live or runtime state. Live state lives inside the system, keyed by
  entity.
- A component describes its own concern only. It never holds references to another feature's data or assets: e.g.
  `NavAgentComponent` does not reference a `VehicleSpec`. A system that needs several concerns views the entities
  that have all of those components. A component may reference an asset that IS its own concern (MeshRenderer ->
  mesh, VehicleComponent -> VehicleSpec).
- Don't multiply components per entity. Shared per-model data goes in a ScriptableObject asset, not a component.
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
  See `parser.cpp parseAttributes()`. Never use comment annotations.
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
