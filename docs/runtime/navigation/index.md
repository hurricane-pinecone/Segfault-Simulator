# Navigation (AI agents)

`NavigationSystem` plans and drives AI agents round a 3D scene: a car going
to the shed, a tractor fetching a trailer, several machines meeting in a yard.
You tell an agent where to go; it finds the way round the scene's colliders on
worker threads, drives it, and keeps out of the others' way by priority when they meet. Only
vehicles are built so far; the code leaves room for walkers later.

It's the standard textbook stack, so nothing in it is specific to one game:

1. a **coarse graph** of the open ground (gateways, aisles) picks the corridor
   for a long trip;
2. a **hybrid A\*** search (Dolgov et al. 2010) finds a drivable way along it,
   its moves continuous-curvature turns (Fraichard & Scheuer 2004; Banzhaf et
   al. 2017), costed in **seconds** (backing up and stopping to change gear
   cost extra), so a three-point turn and a wide turn round are compared by how
   long each takes (never a ring, below). Its heuristic is the largest of: a plain 2D Dijkstra map round the
   solids (Dolgov's holonomic-with-obstacles heuristic); the exact
   continuous-curvature length to the goal ignoring obstacles, within four
   turning radii of it (the non-holonomic-without-obstacles one); and a
   **heading-aware map** that has both at once: hybrid A\* run backwards from
   the goal as a Dijkstra over coarse bins (1 m x 16 directions x gear), with
   the rig's own motions (short straights and arcs, forward and, if it may,
   backing with a cusp's cost at each change). Its arcs turn as the search's
   own turns do on average (each goes in and out of lock, so a run of them
   turns wider than full lock), not held at full lock. Each move keeps the
   reference point off the solids and off ground the cost layer bars to it,
   the body (discs of its inscribed radius along it) off the solids, and,
   where each move starts, the rig's whole outline (its corners) off them,
   asking the cost layer about the pose where the outline is over barred
   ground (a body kept off a crop). A trailer's outline is checked too, once
   the move is as far from the goal as the trailer reaches back (nearer, its
   angle is whatever the way in made it), swung to the angle one arc leaves
   it at. So a rig facing away from the goal knows it has turning to do *and
   that turning round takes room, its trailer's too*: with no room near the
   goal it is priced the way round. That map is built only once the search
   has grown 300 poses (an easy way never pays for it; till then a cheap map
   over cells x 8 directions that charges for turning in place stands in),
   and it runs till the start is reached facing its way, then a little past
   (a bin not reached is at least that far; if the start is never reached,
   it has searched the whole area). If it never gets near the start and runs
   into the edge of the area searched, the way is likely outside that area:
   the search stops at once and the planner widens the area, instead of
   flooding the small one. Where a pose is further from anything solid (or
   ground barred to the body) than its outline's circle reaches, the outline
   isn't looked at point by point (a distance map built with the raster):
   the same answer, open ground passed at a glance. Its moves are worked out
   once per plan,
   and its bins are coarser (1 m, 10 degrees) where nothing solid is near. It
   first tries a plain shot to the goal; then the heuristic maps (and the
   cost layer's raster) are built only if the search must go on, so a plain
   way costs neither. Close to a goal it also shoots to the goal's lined-up
   pose two turning radii out (behind it to drive in, ahead of it to back in)
   and straight in from there, as a driver parks in a bay. Every way is
   priced by **the time it takes** (`VehicleModel::speedAt()`: the gear's top
   speed, slow for a tight curve or a fast-turning wheel, the same function
   the speed profile uses; the time lost pulling away, braking to a stand
   and at every change of direction, `cuspSeconds()`; backing up at
   `reverseSpeed`). A shot that gets there is not taken at once: it is the
   way found so far, and the search goes on while any open pose could still
   beat it (its time so far plus the unweighted heuristic), so a loop is
   driven only when it really is quicker than backing up a little. There is
   no turn-angle cap and no reverse surcharge;
3. a **smoother** shortens it with the same turns, so the wheel never snaps;
4. a **speed profile** slows for tight curves and stops at gear changes;
5. a **follower** drives the path exactly, the agent's pose its path's at the
   distance it has come. It brakes at the same derived rate its speed profile
   was priced with, so it stands exactly on each stop. A stop named nearer
   than its brakes can reach (another agent's body found late on its way) is
   never driven past: it stands there at once and counts as a hard stop
   (`Fleet::counters(id).hardStops`, the fastest in `hardestStop`). Nothing
   priced that stop, so the count shows where traffic warned it too late.

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
| `NavAgentComponent` | it's an agent: how it searches, what it collides with, its rank in traffic and how it prices a route |
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
| `effort` | driving style, a behaviour cap: the share of what its engine, brakes and grip allow that it uses (1: flat out; 0.25: gentle) |
| `layers` | which collider layers it collides with |
| `priority` | rank in traffic: higher reserves its way first (a working machine) |

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

The rig (`nav::vehicle::Rig`) comes from the game's
`setRigProvider(fn(entity) -> Rig)`, asked every update: the mass it moves
besides its own (what it tows and carries: a tractor towing a full trailer
speeds up and stops more slowly), the grip under it (friction coefficient:
tyres x surface), and its wheelbase and fixed axle as measured from its
model, each used only where the spec's (`wheelbase`, `axle`) is 0. The axle
(`Rig::axle`, metres ahead of the entity's origin along +X) is the agent's
reference point: the fleet's poses, paths, boxes and a trailer's
`hitchBehind` are all of it, the same point the game's planners plan, so a
game hands its paths over as they are. Its `effort` is always the
component's.
Without a provider: no load, grip 0.6 (firm, dry ground).

**One spec.** The game's own planners derive their spec with the same
`applySpec(spec, rig)` and the same provider function, so the plan is priced
with exactly the accel, brake, cornering and turning circle the follower
drives by. Values go in unrounded (a changed load re-derives the spec).

**One stopping distance.** `Follower::stoppingDistance(v, brake, dt)`
(`v dt + v^2 / 2b`, and `stopSpeed()` its inverse) is what the follower
stands by; a game's look-ahead or hand-over reach calls it with the agent's
spec brake rather than its own formula.

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

A mounted part (`rigid = true`: a linkage tool, a combine's header) joins
the agent's own boxes. Each of its colliders is measured by
`NavigationSystem::mountedBox()`: its corners bounded in the agent's flat
frame (reference point, heading; heights above the ground), so a header
pitched up by its lift is planned with its nose where the physics has it,
and its bottom `rideDrop` metres lower (driven, the body pitches into ruts
and on its springs). A game's own planners measure mounted tools with the
same function. Call `refresh(agent)` after moving a mounted part (a lift).

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

A goal may also say how it's come to: `arriveGear` (`AgentGoal`'s, the
planner's `vehicle::Goal`'s) is 0 (either way, the
default), 1 (driving in forward) or -1 (backing in: a parking spot a rig is
reversed into, a trailer backed into its row). The last stretch into the goal
is then driven in that gear whatever came before: a rig that could drive
straight into a drive-through bay instead drives past it and backs in. The
search's shots and lined-up approaches, the smoother's last shortcut and the
heading-aware map (seeded only in that gear at the goal) all keep to it; the
speed profile and follower need nothing new (a cusp, then backing to a stand).

```cpp
sfs::nav::AgentGoal spot{{{-8.2f, 32.0f}, 3.14159f}, 0.0f};
spot.arriveGear = -1; // reversed in, its trailer straight behind
nav->requestGoal(semi, spot);
```

| Event | When |
| --- | --- |
| `Arrived` | it's there |
| `Partial` | the search ran out of its first budget: it drives the clear part while it searches on |
| `Blocked` | proven: no way exists. `blockers` says what hems it in; the debug view marks them |
| `Yielding`, `Resumed` | it began standing for `other`'s reservation or zone, and went on |
| `NeedsPlan` | a game's path can't go on as it was (it went to a wait spot, or a static obstacle is on it): plan it again from where it stands |

`state(entity)` says what it's doing now (Idle, Planning, Moving, Yielding,
Arrived, Blocked).

**No "no way" from a time limit.** Searches are budgeted in nodes, never the
clock, so the answer is the same on a fast or a busy machine. Out of budget, an
agent drives the clear way so far and the search goes on with twice the
budget; it only reports `Blocked` when every reachable pose was tried, or at
once when something stands on every goal (no pose there is clear, so none is
ever reached; `blockers` names what's on it). Starting touching something with
a goal within getting out of it is the one exception: that's searched.

A goal in a pocket (clear to stand on, but hemmed in so no way leads to it) is
proven from its own side: when the search from the start hasn't found a way in
its first budget, a search from each goal back to the start runs once with that
same budget. A rig that may reverse and tows nothing moves the same either way,
so if that search grows every pose it can reach without meeting the edge of its
area, the planner reports `Blocked` without flooding the whole map from the
start. A `Blocked` result tells the two apart: hemmed in at the start, it lists
the `blockers`; a goal proven from its side lists none.

Every planner call builds its rig's steering (`Steering`, one per curvature
level). The table of elementary turns each one needs is computed once per
curvature and curvature rate and shared by every later call (it costs
milliseconds), so a stand check or a short plan doesn't pay for it again.

## Ground costs and traffic

A game says what ground costs with `setCostLayer(name, layer)`: a
`nav::CostLayer` gives a factor on time for a pose (1 ordinary ground, a worn
track 0.85, infinity for never, e.g. a combine's body over standing crop) and
where nothing may stop (`noWait`: gateways). Layers multiply.
`setPriorityProvider` overrides each agent's priority.

Agents meeting is the textbook multi-vehicle method AGV and mining fleets
use: **prioritised planning with a space-time reservation table**
(`nav::Traffic`), plus **zones** for narrow places. There is no negotiation
per meeting:

1. **Fixed priorities.** Agents are ranked by priority, then by id: one total
   order. A game sets priorities from a named table of tasks (a working
   combine above the cart serving it, above other field work, above
   travelling); the rank of a pair never flips while their tasks stand.
2. **Reservations.** Each frame, in rank order, each moving agent reserves
   the ground its body sweeps along its path, every `claimEvery` metres for
   `claimSeconds`, each sample in the time window its speed profile has it
   there; its body includes all its boxes (front header, trailer) and its
   **stopping stretch** (`AgentMotion::stoppingDistance()`, the vehicle's
   `Follower::stoppingDistance()`), and its reservation ends where it will
   stand. A lower agent checks its own samples against every higher
   reservation in space and time: where they first meet, it stands at the
   last point of its path where its body is clear of the higher
   reservations (the met one's whole way on, `wayReach`, included) and not
   in a `noWait` place, and goes on once they have passed.
3. **Wait spots.** With nowhere clear on its path, or standing in the way of
   a higher agent that stands for its body (head-on in a lane), the lower
   agent is `inTheWay`: it is planned (the same hybrid A\*) to the nearest
   pose clear of every higher reservation (`Traffic::reservedAbove()`), then
   planned on from there (a game's path is handed back with `NeedsPlan`),
   held by the reservations till they've passed. One that finds no wait
   spot is stuck: a static obstacle the others plan round.
4. **Zones.** Narrow places, where one agent can't pass another as wide as
   itself (its boxes' sides hemmed in by the statics on both sides: a
   gateway, a lane between buildings), are found along each path from the
   colliders, never placed by hand. Each is a mutex: a narrow stretch is
   granted whole (stretches with less than two of its lengths between them
   as one, so it never holds one while waiting for the next), in rank order,
   only while no other agent holds ground it overlaps, and held until its
   body has left it. Nobody enters ground another holds; one already in it
   goes on through. Never hold-and-wait: a stretch it isn't in yet is given
   up while it stands short of it (for another's reservation, body or zone),
   so a higher agent waiting for it gets it. One in a narrow place that a
   higher agent waits for goes first: the waiting one's reservation doesn't
   stop it, only its body does, and then it is in that one's way and goes to
   a wait spot (two carts meeting head-on in a farm lane: never both
   standing for good).
5. **Static obstacles.** An agent with nothing to do (no path, or at its
   end, and no plan on the way), one stood `parkedAfter` not for traffic (a
   stop its game made: a speed cap of 0), and a stuck one are
   `Fleet::solid()`, the one test: a static solid in every plan
   (`Fleet::planWorld()`, a game's planner worlds), never waited on. A path
   into one is planned round it at once (`Why::Solid`; a game's path handed
   back with `NeedsPlan`), standing short of it meanwhile. One whose game
   is planning its next path (`NavigationSystem::setPlanning()`) is about
   to move, so it's never a solid meanwhile: two machines side by side,
   each planning its way off with the other a solid in its plan, would
   search on for ever.

Whatever its rank, nobody drives on into another's body (`Why::Body`); a
standing body is kept the planners' own gap away (`vehicle::Tunables::gap`,
2 cm). A higher agent standing short of a lower one's body reserves only up
to there, so a lower one already crossing in front of it carries on through
rather than stopping in front of it.

`Traffic::scheduleOf(id)` says what an agent does this frame and why: `Why`
(None, Reserved, Zone, Body, Solid), where it stands (`stopAt`), for whom
(`by`) and whether it's `inTheWay`. A game's stuck log can print it.

```cpp
fleet.add(2, spec, parkedPose);  // nothing to do: fleet.solid(2) is true
fleet.requestGoal(1, goal);      // planned round it, no waiting on it
fleet.setPriority(3, 4);         // a working combine: everyone keeps out
```

`cooperate(a, b, true)` exempts a pair that keep their own spacing (a cart
running beside a combine).

## A game's own paths and checks

- `setPath(entity, path)` drives a path the game laid itself (a field pass),
  claimed and given way to like any other.
- `planNow(entity, ask)` plans synchronously on the calling thread, for a
  game's own checks on its own worker.
- `nav::vehicle::planNow(PlanRequest)` is the vehicle planner on its own, for
  tools that don't want a scene.
- `nav::vehicle::touches(request, pose, hitch)` is THE stand-here check: what
  the vehicle standing at `pose` touches (its boxes plus `Tunables::gap`; its
  trailer at `hitch` when given). The planner starts by it and refuses a goal
  by it ("something stands where it's to stop"), so a game choosing a spot to
  stop at asks this with the request it plans with, never a check of its own:
  a spot it passes is one the plan can reach.
- `nav::vehicle::touchesAlong(request, from, hitch, segments, endIn)` is THE
  check of a way the game laid itself (a field pass, its own eased curves
  sampled into segments, a straight onto a hitch): the planner's own final
  check (its boxes plus `Tunables::gap`, a pose every `Tunables::exactStep`,
  the trailer swung on its hitch and kept within its limits), either gear.
  Getting out of a touch at the start is let off within `kEscapeReach`, as
  `planNow()` lets a way out; with `endIn`, where the end itself touches
  (backing onto what it's to hitch), its last `kEscapeReach` metres are let off
  too. It returns where the way first touches (metres along it), or nothing
  when it's clear. A game never checks its own ways with a check of its own:
  the planner's ways and the game's are clear by the same rule.
- A game's bespoke manoeuvres (a field's headland turns, going round what
  stands on a pass) are planned with `planNow()` too, with their own
  `CostLayer` (stay on the headland, keep near the pass's line) and a small
  first `nodeBudget`: a shot (a U-turn, a bulb turn, a three-point turn) where
  one's clear, else the search. Plan and drive the same request and both get
  the same way.
- `setPath()` follows the path from its start: hand over a path that starts
  where the agent is. A game planning while its agent moves plans from where it
  comes to a stand (the path at `along() + Follower::stoppingDistance()`) and
  hands the path over once it has stood.

## A player at the wheel

A game lets the player drive an agent (first person, say) through the same
system, so the player's vehicle moves by exactly the physics the AI's paths
are driven by:

```cpp
nav.takeWheel(tractor);                 // its goal or path dropped
nav::Controls c;
c.throttle = 1.0f;                      // 0..1
c.brake = 0.0f;                         // 0..1
c.steer = 0.5f;                         // -1..1 of full lock, + left
c.gear = 1;                             // 1 forward, -1 backing up
nav.setControls(tractor, c);            // each frame
nav.setSpeedCap(tractor, workingPace);  // still holds
nav.handBack(tractor);                  // stands where it is
```

Each update the system moves it by the last controls given, by its kind's own
motion (`AgentMotion::takeWheel()`; the vehicle's is `nav::vehicle::HandDrive`),
and writes its transform and its trailer's as for any agent. `HandDrive` is
built from the agent's own `VehicleModel` (`modelOf(spec)`: the spec derived by
`applySpec()`), so nothing in it is a number of its own:

- the wheel turns toward the steering asked for at `steerRate`, never past
  full lock (from `wheelbase` and `minRadius`);
- the throttle asks for its share of the gear's top speed, never more than
  `VehicleModel::speedAt()` allows on the curve it's on (the speed profile's
  own limit) or the caller's speed cap, reached at the derived `accel`; let
  off, the drive holds it back to that at the same rate (the drivetrain
  carries the same force either way);
- the brake adds the derived `brake` rate;
- asked for the other gear while moving, it brakes to a stand, then stands
  `shuttleSeconds` (as the follower does at a cusp) before it moves off;
- a trailer swings on its hitch by `hitchStep()`, within its limits.

While it's driven by hand it's solid to every other agent (`Fleet::solid()`):
where it goes next is anyone's guess, so the others plan round where it is and
are planned again if it gets in their way. `requestGoal()` or `setPath()` on
it hands it back first. `Fleet::takeWheel()`, `setControls()`, `handBack()`
and `handDriven()` are the same without entities (tests, tools).

## Debug view

`settings()` offers **Draw paths** (green forward, orange backing, the goal in
blue), **Draw claims** (each agent's next seconds; yellow while it stands for a higher reservation or a zone, or goes to a wait spot) and
**Draw blocked ways** (red: the agent and what blocks it). They draw into
`RenderSystem::debugLines()` and, like every debug feature, only while
`debugFeatures::enabled()`: in the editor, Debug builds and development
exports, never in a release export.

## What's solid, in 2D

`nav::CollisionSpace` answers "would these boxes touch anything?" from the
frozen colliders exactly. When something needs the colliders as outlines
instead (a field planner tracing obstacles, a game's cells, a 2D heuristic),
it takes them from `nav::footprints(world, layers, ignore, ground)` or
`CollisionSpace::footprints()`: each collider's exact footprint as a
`BoxSpace::Solid` (a box's outline yawed as it's turned, the upright box round
it when tipped; a sphere's or capsule's circle), with its height band over the
ground and its body's id. It's the one place a collider's 2D outline is worked
out: `CollisionSpace::raster()` uses it too, so nothing keeps a copy of its own.

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
- M. Erdmann, T. Lozano-Perez, *On Multiple Moving Objects*, Algorithmica
  1987 (prioritised planning).
- D. Silver, *Cooperative Pathfinding*, AIIDE 2005 (space-time reservations).
