# Navigation (AI agents)

`NavigationSystem` plans and drives AI agents round a 3D scene: a car going
to the shed, a tractor fetching a trailer, several machines meeting in a yard.
You tell an agent where to go; it finds the way round the scene's colliders on
worker threads, drives it, and gives way to the others when they meet. Only
vehicles are built so far; the code leaves room for walkers later.

It's the standard textbook stack, so nothing in it is specific to one game:

1. a **coarse graph** of the open ground (gateways, aisles) picks the corridor
   for a long trip;
2. a **hybrid A\*** search (Dolgov et al. 2010) finds a drivable way along it,
   its moves continuous-curvature turns (Fraichard & Scheuer 2004; Banzhaf et
   al. 2017), costed in **seconds** (backing up and stopping to change gear
   cost extra), so a three-point turn and a loop are compared by how long each
   takes. Its heuristic is the larger of two Dijkstra maps from the goal:
   plain 2D round the solids, and one over cells x 8 directions of travel that
   charges for turning, so a rig facing away from the goal, or about to come
   at it from the wrong side, knows it has turning to do. Its moves are worked
   out once per plan, and its bins are coarser (1 m, 10 degrees) where nothing
   solid is near;
3. a **smoother** shortens it with the same turns, so the wheel never snaps;
4. a **speed profile** slows for tight curves and stops at gear changes;
5. a **follower** drives the path exactly, the agent's pose its path's at the
   distance it has come.

## Turning it on

List it after `PhysicsSystem3D` among the scene's systems (Scene Settings →
Systems → **Add System** → *Engine:* NavigationSystem, or
`"systems": ["PhysicsSystem3D", "NavigationSystem", ...]` in the scene file).
Like physics it only exists while the scene plays; ask for it with
`scene().tryGetSystem<sfs::NavigationSystem>()`.

## An agent

Give the entity:

| Component | Why |
| --- | --- |
| `NavAgentComponent` | it's an agent: how it searches, what it collides with, its right of way and how it prices a route |
| `VehicleComponent` (kind `"vehicle"`) | its model: an `AssetRef<sfs::VehicleSpec>` (a `.asset` shared by every machine of the model) |
| `BoxCollider3DComponent`s (on it or entities below it) | its shape: the planner checks these exact boxes, heights included |
| `RigidBody3DComponent`, `kinematic` on | so physics sees it move (turn on `touchStatic` to hear what it touches) |

A vehicle agent with no `VehicleComponent`, or one whose spec is unset or
can't be read, is left out with an error in the log.

`NavAgentComponent`'s fields (navigation only, no vehicle numbers):

| Field | Meaning |
| --- | --- |
| `kind` | the agent kind: `"vehicle"` |
| `search` | the path search, by name: `"hybridAStar"` |
| `reversePenalty` | route cost: a second backing up counts this many. A change of direction is priced at its real time (`VehicleModel::cuspSeconds()`): the VehicleSpec's `shuttleSeconds` standing plus the time lost braking to a stand and pulling away, from the same derived accel and brake the follower drives with. There is no separate planner price. |
| `effort` | driving style, a behaviour cap: the share of what its engine, brakes and grip allow that it uses (1: flat out; 0.25: gentle) |
| `layers` | which collider layers it collides with |
| `priority` | right of way: higher goes first (a working machine) |

### VehicleSpec: properties, never outcomes

`sfs::VehicleSpec` (a scriptable object) holds what a vehicle model IS:

| Field | Meaning |
| --- | --- |
| `wheelbase`, `maxSteerAngle`, `steerRate` | m; degrees the steered wheels turn at most; degrees a second they turn |
| `topSpeed`, `roadSpeed`, `reverseSpeed` | m/s: what its gearing gives, what it travels at between jobs, backing up |
| `power`, `mass`, `brakeForce` | kW at the wheels, kg empty, kN its brakes hold back |
| `shuttleSeconds` | driving: how long it stands at a change between forward and reverse (default 0.5 s; the wheel goes on turning as it moves off) |
| `axle` | the middle of its fixed (rear) axle in the model's frame: the point it turns about. It faces +X |
| `mayReverse` | whether it can back up at all |

How it moves is derived from those, per agent, and derived again whenever
what it depends on changes:

| Outcome | From |
| --- | --- |
| tightest turn | `wheelbase / tan(maxSteerAngle)` |
| speeding up | power over the whole rig's mass (`2 P / (m v)` averaged to road speed), no more than the grip on its own weight allows |
| braking | `brakeForce` over the whole rig's mass, no more than the grip allows (the follower brakes and pulls away at these derived rates too) |
| cornering | grip x g |
| each of them | x `effort` |

The rig's mass is the spec's `mass` plus what the game reports with
`setLoadMass(agent, kg)` (on hitching, unhitching, filling, emptying): a
tractor towing a full trailer speeds up and stops more slowly. The grip
(friction coefficient: tyres x surface) comes from
`setGripProvider(fn(entity) -> float)`; without one it's 0.6, firm dry
ground.

A game extends the spec with its own kinds of machine, and a VehicleComponent
takes their assets:

```cpp
struct TractorSpec : sfs::VehicleSpec
{
  float hitchHeight = 0.5f;
  float linkageLiftRate = 0.3f;
};
```

Hitching is live, not data: `hitch(agent, {towed, hitchBehind, drawbar,
forwardLimit, reverseLimit})` and `unhitch(agent)`. The towed entity's
colliders are its box and its origin is its axle; the system plans the rig
and writes the towed entity's transform.

The system writes the agent's transform (and its trailer's) while it drives.
Don't move it yourself meanwhile; move it while it stands and it plans from
there next time.

## Sending it somewhere

```cpp
auto* nav = scene().tryGetSystem<sfs::NavigationSystem>();
nav->requestGoal(tractor, {{{40.0f, 12.0f}, 1.57f}}); // position, heading
```

`requestGoals` takes several goals (it goes to the quickest) and an optional
goal test ("anywhere clear of the gate"). Each request has an id; the last one
counts. Then read `events()` each frame:

| Event | When |
| --- | --- |
| `Arrived` | it's there |
| `Partial` | the search ran out of its first budget: it drives the clear part while it searches on |
| `Blocked` | proven: no way exists. `blockers` says what hems it in; the debug view marks them |
| `Yielding`, `Resumed` | it began giving way to `other`, and went on |

`state(entity)` says what it's doing now (Idle, Planning, Moving, Yielding,
Arrived, Blocked).

**No "no way" from a time limit.** Searches are budgeted in nodes, never the
clock, so the answer is the same on a fast or a busy machine. Out of budget, an
agent drives the clear way so far and the search goes on with twice the
budget; it only reports `Blocked` when every reachable pose was tried.

## Ground costs and right of way

A game says what ground costs with `setCostLayer(name, layer)`: a
`nav::CostLayer` gives a factor on time for a pose (1 ordinary ground, a worn
track 0.85, infinity for never, e.g. a combine's body over standing crop) and
where nothing may stop (`noWait`: gateways). Layers multiply.
`setPriorityProvider` overrides each agent's priority (a machine at work goes
first).

When two agents' next seconds overlap, the one with the lower priority (then
whoever claimed that stretch later) gives way, and only it acts:

- it **holds short** where its whole body is clear of the other's way, never
  in a `noWait` place, and goes on once the other has passed;
- already in the other's way (head-on in an aisle), it **clears out**: it plans
  to the nearest place off the other's way, waits there, then plans on;
- held by an agent that has **parked**, it plans round it (an agent standing a
  couple of seconds is a solid to the others' plans, whether or not it still
  holds a path; a moving one never is).

The one with the right of way only stops while the other's body is actually
on its way, and only until that one has parked there: then it plans round it
(a game's own path is handed back with `NeedsPlan`). So a pair never wait on
each other for good, even when the one giving way is held where it stands (a
speed cap of 0, a clear-out it can't make). Two agents never re-plan into each
other.

`cooperate(a, b, true)` exempts a pair that keep their own spacing (a cart
running beside a combine).

## A game's own paths and checks

- `setPath(entity, path)` drives a path the game laid itself (a field pass),
  claimed and given way to like any other.
- `planNow(entity, ask)` plans synchronously on the calling thread, for a
  game's own checks on its own worker.
- `nav::vehicle::planNow(PlanRequest)` is the vehicle planner on its own, for
  tools that don't want a scene.

## Debug view

`settings()` offers **Draw paths** (green forward, orange backing, the goal in
blue), **Draw claims** (each agent's next seconds; yellow while giving way) and
**Draw blocked ways** (red: the agent and what blocks it). They draw into
`RenderSystem::debugLines()` and, like every debug feature, only while
`debugFeatures::enabled()`: in the editor, Debug builds and development
exports, never in a release export.

## Adding a kind or a search

Agent kinds come from a registry: `nav::AgentKinds::add("walker", kind)` with
an `nav::AgentKind` (how it plans, follows and takes up room) adds one without
touching the system. Each kind's path search is swappable the same way: the
vehicle kind's searches are `nav::vehicle::searches()`, `"hybridAStar"` built
in; a search meets `nav::SearchBase`'s contract (node budgets, continuing where
it stopped, and never claiming "no way" it can't prove: a sampling search
answers Unknown, never Exhausted).

| Folder | What |
| --- | --- |
| `engine/runtime/navigation/` | shared by every kind: `Space` (what's solid), `CollisionSpace` (from engine colliders), `CostLayer`, `CoarseGraph`, the search contract, `Traffic`, `Fleet` (the agents together), `AgentKind` |
| `engine/runtime/navigation/vehicle/` | the vehicle kind: model, steering, hybrid A\*, smoother, speed profile, follower, planner |
| `engine/runtime/systems/navigationSystem.h` | the System |

The shared folder never includes the vehicle one.

## References

- D. Dolgov, S. Thrun, M. Montemerlo, J. Diebel, *Path Planning for
  Autonomous Vehicles in Unknown Semi-structured Environments*, IJRR 2010
  (hybrid A\*).
- T. Fraichard, A. Scheuer, *From Reeds and Shepp's to continuous-curvature
  paths*, IEEE T-RO 2004.
- H. Banzhaf, N. Berinpanathan, D. Nienhüser, J. M. Zöllner, *From G² to G³
  continuity: continuous curvature rate steering functions for sampling-based
  nonholonomic motion planning*, IV 2018 / *Hybrid curvature steer*, 2017.
- D. Silver, *Cooperative Pathfinding*, AIIDE 2005 (space-time reservations).
