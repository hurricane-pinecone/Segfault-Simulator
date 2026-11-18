# Engine facilities

Game code reaches the engine's features (raycasts, path orders, render
modules, debug lines, prefab copies) through its **facilities**: named
accessors on every `sfs::System`, `sfs::Service`, `sfs::GameObject` and
`sfs::Scene`, as Unity has `Physics.Raycast`, `NavMeshAgent.SetDestination` and
`Debug.DrawLine`.

```cpp
#include <sfs/sfs.h>

class TurretSystem : public sfs::System
{
protected:
  void create() override { registerComponent<Aim>(); }

  void update(double) override
  {
    const sfs::facility::Physics3D world = physics();
    const sfs::facility::DebugDraw lines = debugDraw();

    for (auto [turret, aim] : getEntities<Aim>())
    {
      sfs::Ray ray;
      ray.origin = aim.origin;
      ray.direction = aim.direction;

      if (const auto hit = world.raycast(ray, aim.range))
        lines.line(aim.origin, hit->point, glm::vec4(1, 0, 0, 1));
    }
  }
};
```

A facility call is the engine's API, answered at once: an order given to the
navigation reads back in the same step, a raycast answers now. It isn't an
[event](../../core/events/index.md): events are for what game systems and
services tell each other. How an engine part's own work turns out comes back as
news where it takes time (`nav::AgentEvent`: an agent arrived, is blocked, gives
way).

Game code never looks a system up (`getSystem`, `tryGetSystem`, `hasSystem`,
`forEachSystem`): engine features come from the facilities, game state from
components and scene data, and what one part tells another goes by event. The
`system-lookup` lint flags a lookup in a project's code; lookups are for hosts
and tools (the game loop, the editor, tests).

## The facilities

Each accessor returns a small handle in `sfs::facility::`, declared in
`engine/runtime/facilities/<name>.h` (all of them come with `<sfs/sfs.h>`).

| Accessor | Behind it | Queries | Commands |
| --- | --- | --- | --- |
| `physics()` | `PhysicsSystem3D` | `raycast(ray, max)`, `snapshot(include)`, `revision()`, `hasBody(e)`, `velocity(e)`, `angularVelocity(e)`, `gravity()` | `addForce`, `addImpulse`, `setVelocity`, `setAngularVelocity`, `setGravity` |
| `navigation()` | `NavigationSystem` | per agent: `isAgent`, `state`, `solid`, `handDriven`, `pose`, `speed`, `along`, `curvature`, `hitchAngle`, `path`, `spec`, `counters`, `footprint`, `schedule`, `ranksAbove(a, b)`, `ahead`; the world: `settings()`, `coarseGraph()`, `coarseEdges()`, `laneGraph(name)`, `scheduler()` | per agent: `requestGoal(s)`, `setPath`, `cancel`, `setControls`, `takeWheel`, `handBack`, `setPlanning`, `cooperate`, `refresh`, `planAhead`, `planNow`; the world: `setLaneGraph`, `setCoarseEdges`, `setCostLayer`, `setGround`, `setPriorityProvider`, `setRigProvider`, `setSpace` |
| `graphics()` | `RenderSystem` | `module<T>()` (the engine's render modules: grass, deformation, puddles, particles, UI...; or a game's own), `stats()` | `addModule<T>(args...)` (a game's own module), `updateTexture(texture, from, to)` |
| `meshes()` | the scene's `MeshLibrary` | `find(name)`, `nameOf(mesh)`, `bounds(mesh)`, `positions(mesh)`, `raycast(mesh, world, ray)`, `pickEntity(ray)`, `pickSurface(ray)` (what a ray meets, by the meshes as drawn) | `add(mesh, name)`, `remove(mesh)`, `resolve(name)`, `setMaterial(name, material)` |
| `cameras()` | `CameraSystem` (`active()`), `RenderSystem` (the rest) | `active()` (the active camera entity), `rendered()` (the camera as last drawn), `screenRay(pixel)` | |
| `debugDraw()` | the frame's `DebugLines` | `enabled()` | `line`, `box`, `circle`, `sphere`, `capsule`, `forceColliders(on)`; `DebugDraw::pack(colour)` |
| `prefabs()` | `PrefabSystem` | | `spawn(prefab, transform, parent)`, `despawn(root)` |
| `animation()` | `AnimationSystem` | `clips(animator)`, `clipNames(animator)`, `length(animator, clip)` | `play(animator, clip, fade)`, `restart(animator)` |
| `assets()` | the scene's `AssetStore` | | `requestModel(id, &model)` (loaded, loading or failed) |

What the engine keeps on a component is written there, not called:

- an agent's hitch, speed cap and whether it's an agent now are
  `NavAgentComponent::hitch`, `speedCap` and `active`
  ([navigation](../navigation/index.md));
- an animator's steady state is its `AnimatorComponent` (`playing`, `time`,
  `speed`; [animation](../animation/index.md)); a one-off cross-fade or
  restart is `animation().play()` or `restart()`;
- a camera feed's picture is `CameraFeedComponent::picture`, made each frame
  before the objects run.

## Who may use them

- **Systems**: all of them, in `create()`, `update()`, `lateUpdate()`, the
  hooks and their event handlers.
- **Services**: the same, in their handlers: a raycast, a path order, a prefab
  copy. A facility hands out answers, never lists of entities. A service may
  read an entity a facility query gives (a raycast's hit) and what it leads to
  (its parent, its parts), but it changes only the entities its events hand
  it (and those it makes itself). A service has no update, so a
  debug line it draws shows for one frame: drawing every frame is a System's
  job.
- **Game objects** (UI layers included): the same, as their scene's.
- **Scenes**: the same, public: a scene's own code (its `onInit()`, its
  `onUpdate()`) and the host that runs it.
- **Utils** take what they need as a parameter: a facility by value
  (`sfs::facility::Navigation navigation`), or what it handed out (a
  snapshot, a module), never a registry or a scene to get it from.

```cpp
// A util: what it needs comes in.
bool clearAhead(sfs::facility::Physics3D physics, const sfs::Ray& ray, float reach)
{
  return !physics.raycast(ray, reach);
}
```

## Lifetime and threads

A facility is a value handle (a pointer or two), made where it's asked for at
the cost of one lookup in a fixed table: get it where it's used.

- **Valid for the step that got it.** Like a `string_view`, keep it in no
  member: the `keeps-facility` lint flags a facility field in a system,
  service, game object, scene or component. A plain struct a util takes,
  holding facilities for one call, is fine.
- **The scene's thread only**: system updates, service handlers, object
  callbacks. A worker gets what a facility hands out to share, which is
  immutable and ref-counted: `physics().snapshot()` (a `CollisionWorld3D`),
  `navigation().scheduler()`, `navigation().laneGraph(name)`,
  `navigation().coarseGraph()`. A job copies those, never the facility.
  Debug drawing is scene-thread only.
- **Some answers point into the engine and are valid for this step only**:
  `navigation().path(agent)`, `navigation().spec(agent)`,
  `navigation().settings()`, `navigation().coarseEdges()`,
  `graphics().module<T>()`, `graphics().stats()`, `meshes().bounds(mesh)`,
  `meshes().positions(mesh)`, `cameras().rendered()`. Compare or read them
  now, never keep them. Anything that may outlive the step is a `shared_ptr`.
- **Take it once before a loop.** Getting a facility is cheap, but not free (a
  lookup each time): in a loop over many entities, take it into a local first,
  `const sfs::facility::Physics3D world = physics();`.

```cpp
void update(double) override
{
  // A job gets the snapshot and the scheduler, never the facilities.
  std::shared_ptr<const sfs::CollisionWorld3D> solids = physics().snapshot();
  std::shared_ptr<sfs::nav::PlanScheduler> scheduler = navigation().scheduler();

  if (scheduler)
    m_job = scheduler->submitOne(owner, "fit check", deadline,
        [solids](const std::atomic<bool>&) { return std::any(fits(*solids)); });
}
```

## When the engine part isn't there

A scene may leave an engine part out (no physics, no navigation; a tool or a
test with no renderer). Its facility is then absent:

- its queries give empty answers: `nullopt`, a null pointer, `false`, an
  invalid entity, zero, or the default settings;
- its commands do nothing;
- the first use logs one warning naming the missing part (`physics() used
  where there's no PhysicsSystem3D`), once per process (in the editor, once a
  session, whichever Play first hits it);
- `available()` tells code that does something else without it:

```cpp
if (!navigation().available())
  return; // a scene without agents
```

Systems are made in the order the scene lists them, so in your `create()` an
engine system listed after yours isn't there yet, and its facility is absent:
a command given then is dropped. Set up through it in your first update
instead, behind `available()`.

`debugDraw()` is different: it's off, and silent, whenever debug features are
off (a release export, where its calls compile away) or the scene doesn't
render. `enabled()` says whether it draws, so code can skip working out what
it would draw.

## Reference

- `engine/runtime/facilities/`: `physics3D.h`, `navigation.h`, `graphics.h`,
  `meshes.h`, `cameras.h`, `debugDraw.h`, `prefabs.h`, `animation.h`,
  `assets.h`.
- `engine/core/ecs/facilityAccess.h`: the accessors' declarations.
- The lints: `system-lookup` and `keeps-facility`
  ([sfs-lsp](../../editor/sfs-lsp/index.md#facilities)).
