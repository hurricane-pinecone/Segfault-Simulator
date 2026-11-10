# Lane graphs (roads)

A long trip along roads (or tracks, aisles: whatever a game calls its lanes)
is the textbook split: a **graph search** over the lanes finds which to take
(milliseconds), the vehicle drives each lane's own line with no search, and
the **hybrid A\*** planner searches only the short windows between: getting
on, round a corner at a junction, getting off.

The engine knows nothing about roads. The game decides the graph and hands it
over; the engine searches it and joins it to the vehicle planner.

## The graph: `sfs::nav::LaneGraph`

`engine/runtime/navigation/laneGraph.h`.

- **Nodes**: junctions and ends (`addNode(at)`).
- **Lanes**: DIRECTED, from one node to another (`addLane(Lane)`), each with
  - `line`: the polyline it's driven along, from `from`'s end to `to`'s (a
    lane's centreline, on whichever side the game keeps to: a two-way road is
    two lanes, one each way, each on its own side);
  - `speed`: the most it's driven at (m/s);
  - `cost`: what a metre of it costs (a factor; below 1 preferred);
  - `tag`: the game's own (which road it belongs to).
- Lanes meet at nodes: a lane ending at a node goes on along any lane leaving
  it. A one-way street is one lane.

`route(from, to, options)` finds the way:

1. onto the lanes near `from` (the nearest, and any within a few metres of it:
   both ways along a road; the search picks), off at those near `to`;
2. A\* over the nodes: getting on costs the metres to the lane, each lane its
   length times its `cost`, getting off the metres from it; the heuristic is
   the straight line at the cheapest cost;
3. the lanes taken, sampled every `Options::step`, cut into **stretches** to
   drive without a stop: lanes running on straight are joined; at a corner
   (turned more than `Options::corner`) each side is cut back by
   `radius * tan(turn / 2) + 3 m` to leave room for the turn; the first
   stretch starts `Options::lead` metres on from where it gets on, the last
   ends `lead` short of where it gets off (room to turn on and off).

Nothing comes back (plan the trip as any other way) when either end is
further than `Options::reach` from every lane, when getting on and off is at
least `Options::worthIt` of the way straight there, or when there's no way
through (one-way lanes the wrong way).

A 40 x 40 grid of roads (6,240 lanes) is searched in about 20 ms in a Debug
build (`laneGraphTests`).

## Handing it over

```cpp
auto graph = std::make_shared<sfs::nav::LaneGraph>();
const int a = graph->addNode({0, 0}), b = graph->addNode({400, 0});
graph->addLane({a, b, eastLine, 11.0f, 1.0f, roadId}); // keeping left
graph->addLane({b, a, westLine, 11.0f, 1.0f, roadId});
nav->setLaneGraph("roads", graph);  // replaced whole when the lanes change
```

`NavigationSystem::setLaneGraph(name, graph)` registers (or with null removes)
a graph by name; `laneGraph(name)` hands it back. A graph is immutable once
handed over: plans hold a `shared_ptr` to the one they started with, so
replacing it never disturbs a plan in progress.

## The trip: `vehicle::planLaneTrip()`

`engine/runtime/navigation/vehicle/laneTrip.h`.

```cpp
sfs::nav::vehicle::LaneTripRequest trip;
trip.plan = request;          // the vehicle, Space, CostLayer, from, goal
trip.lanes.radius = 6.0f;     // the rig's turning circle
trip.lanes.lead = 13.6f;      // room to turn on and off
trip.reverseOn = false;       // may getting on back up? (corners never do)
trip.reverseOff = true;       // backing into the spot at the end
const auto planned = sfs::nav::vehicle::planLaneTrip(*graph, trip);
```

The parts come back in order: a window (`Part::window`: `On`, `Corner`,
`Round`, `Off`; its `PlanResult` in `found`, where it went in `to`) or a lane stretch
(`Part::stretch`). Each window is a plain `planNow()` of the trip's own
request (the same vehicle, Space and CostLayer: one model), from where the
part before ended; a game may wrap each window's search (`trip.search`: its
own logging, its own retry) but never checks anything the planner didn't.

**Every stretch is checked whole**, as it's driven, by the planner's own check
of a laid way (`touchesAlong()`: the rig's boxes, the one gap, its trailer
swung on its hitch from its angle where the stretch starts; the trailer's
angle is carried along the stretch and into the next window, `Part::hitch`).
The lane graph is the game's plain geometry: nothing keeps a lane line clear
of a tree beside the road, or of a trailer cutting in on a bend. Where a
stretch touches something, the trip leaves the lane there (the textbook local
repair of a global route): the stretch ends a `lead` short of the touch, a
`Round` window (forward, hybrid A*) goes round it and back onto the lane a
`lead` past it where the rig stands clear, and the rest of the stretch is
checked the same way (`LaneTrip::rounds` counts them). The game's look-ahead
checks the same points with the same function, so the two never disagree,
and nothing that touches is ever handed back: planning the trip again never
returns the same blocked way.

A window that finds no way ends the trip there: `state` is that window's
(Blocked or out of budget), the parts before it are good to drive up to, and
the rest is planned again from there when it's reached. `graphMs` and
`windowMs` say where the time went.

A game prices its lanes in the windows with a `CostLayer` of its own (its own
lane cheaper than the other, the verge dearer), passed in `plan.cost` with
the rest of its ground's costs.
