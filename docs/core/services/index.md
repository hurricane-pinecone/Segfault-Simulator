# Services

A **service** (`sfs::Service`) is logic that runs only when its events bring
it something. It has no update and never looks through the registry: it
reacts, in the handlers of the consumers and requesters it makes, to what
arrives, and works on the entities those events hand it.

Where a [system](../ecs/index.md#systems) is the logic that goes over a list
of entities every step, a service is the logic that waits to be asked or told:
handing out work, answering requests with a job, keeping score.

## Service or System

There are three ways for one part of a game to get another to do something,
and a service fits two of them:

- **Ongoing work an entity does** is a task component on it, which its System
  polls each step (`Query<Guard, PatrolTask>`): the System moves the guard,
  writes the task's progress, and sends news when it's done.
- **Asking for a result** is a [request](../events/index.md#requests-and-responses):
  a service can answer it at once, or start a job that answers later.
- **Telling others** is news: a service hears it and acts.

So a service fits work that only reacts: assigning tasks as entities become
free, answering requests through jobs, keeping a tally. Work over lists of
entities, or that needs time to pass (a delay, a timeout, a cooldown), is a
System's. These are ways to use it, not rules: the engine's only rule is what
a service can reach (below).

## Writing one

```cpp
#pragma once
#include <sfs/sfs.h>

struct [[sfs::event_entity_news]] EnemyDiedEvent {};

struct [[sfs::scene_data]] Score
{
  int kills = 0;
};

// Counts the kills on the scene's one Score.
class ScoreService : public sfs::Service
{
protected:
  void create() override
  {
    m_deaths = consumer<EnemyDiedEvent>().onEvent(
        [this](const sfs::Entity&, const EnemyDiedEvent&)
        {
          if (Score* score = sceneData<Score>())
            ++score->kills;
        });
  }

private:
  sfs::EventConsumer<EnemyDiedEvent> m_deaths;
};
```

- `create()` runs once, as the service is added. It makes the service's
  consumers and requesters, which it keeps as members, and builds what it
  builds once (a scheduler it owns). Handles made anywhere else are refused
  and logged. A service must have a `create()`: one without would never run,
  so it doesn't compile.
- Each consumer needs a handler (`onEvent()`, `onEvents()`, `onLatest()` or
  `onLatestEvents()`): a service has no update to read it in, so a consumer
  without one doesn't compile. A handler may `read()` the service's other
  consumers.
- A requester is made as a system's is, `requester<R>()`, with its response
  handlers.
- It sends with `send(e)`, `send(entity, e)`, `cancel<E>(entity)` and
  `broadcast<E>()`, as a system does.
- `setEnabled(false)` stops it running (debug toggling); its consumers keep
  their places, so it misses nothing.

## What it can reach

A service works on:

- **The row's entity**: the entity entity news or a request is about.
- **The entities named in the event's fields** (and in a response's).
- **What those lead to**: the entities their components name in entity
  fields, their parent and children (`sfs::hierarchy::parentOf()`,
  `childrenOf()`, `descendants()`).
- **The scene's data**, `sceneData<T>()` ([scene data](../ecs/index.md#scene-data)):
  a clock, the weather, or the service's own state. A service may keep its
  state in scene data as its one writer, taking it mutable with
  `sceneData<T>()`: others read it there with `sceneData<const T>()` and hear
  the service's news.

On those it reads and changes components as any code does: `getComponent`,
`hasComponent`, `matches`, `addComponent`, `addTag`, `removeComponent`. A
handler's `const sfs::Entity&` is a handle like any other: its constness is
the handle's, not the entity's.

It can also make and destroy entities, `createEntity()` and
`destroyEntity(entity)`: a new one joins the lists at the next flush, as one
a system makes. It can add its own scene data in `create()`, or remove it
(`removeSceneData<T>()`). Scripts start after the scene file has loaded, and
the scene may already hold it (added in Scene Settings), so check first:

```cpp
void create() override
{
  if (!sceneData<Score>())
    addSceneData<Score>();
  // ... its consumers
}
```

What it can't reach is a list: it has no registry, can't query, has no
entities of its own and can't observe a list. A reverse lookup ("who carries
this pickup?") needs a list, so what a service needs must come in the event,
or be stored on the side it's handed.

A component a service adds or removes changes the lists at once, as a
system's change does: systems' hooks and observers run inside its handler.
Adding a component the entity already has replaces it in place, with no join
or leave, so no hook or observer runs; a system polling it sees the new value.

### Dead entities

News about an entity that has since gone is still news: a dying enemy's own
`EnemyDiedEvent` must reach its consumers. So a handler that changes the
row's entity checks it first, or uses `onLatest()`, which skips entities
destroyed since:

```cpp
m_hits = consumer<HitEvent>().onEvent(
    [](const sfs::Entity& target, const HitEvent& hit)
    {
      if (!target)
        return; // gone: getComponent gives null, an add is refused and logged
      if (Health* health = target.getComponent<Health>())
        health->current -= hit.damage;
    });
```

## A coordinator

A coordinator hands out tasks. A task is a component the coordinator adds;
the System of that task works it and sends news when it's done; the
coordinator then gives the next by adding over the old one:

```cpp
struct PatrolTask { sfs::Entity route; float progress = 0.0f; }; // progress: PatrolSystem's
struct [[sfs::event_entity_news]] GuardIdleEvent {};
struct [[sfs::event_entity_news]] PatrolFinishedEvent {}; // sent by PatrolSystem

class SquadService : public sfs::Service
{
protected:
  void create() override
  {
    m_idle = consumer<GuardIdleEvent>().onLatest(
        [](const sfs::Entity& guard, const GuardIdleEvent&)
        {
          if (const Guard* state = guard.getComponent<Guard>())
            guard.addComponent<PatrolTask>(PatrolTask{state->firstRoute});
        });
    m_done = consumer<PatrolFinishedEvent>().onLatest(
        [](const sfs::Entity& guard, const PatrolFinishedEvent&)
        {
          const Guard* state = guard.getComponent<Guard>();
          if (!state)
            return;
          // One add replaces the task in place: PatrolSystem, polling its
          // tasks, sees the new route and fresh progress.
          guard.addComponent<PatrolTask>(PatrolTask{state->nextRoute});
        });
  }

private:
  sfs::EventConsumer<GuardIdleEvent> m_idle;
  sfs::EventConsumer<PatrolFinishedEvent> m_done;
};
```

`PatrolSystem` registers `sfs::Query<Guard, PatrolTask>`, moves its guards
along each one's route, writes `progress`, and sends
`send(guard, PatrolFinishedEvent{})`; it never knows the coordinator. A guard
is between tasks for exactly one step.

## A planner

A service that answers requests with slow work owns its scheduler, built in
`create()`, and starts a job per request in its handler. The job answers
through the Responder it copied, from its worker: progress, then success or
failure. The request carries a snapshot of what it plans over, so the
service reads nothing else and nothing waits.

```cpp
class PathPlannerService : public sfs::Service
{
protected:
  void create() override
  {
    m_scheduler = std::make_unique<sfs::nav::PlanScheduler>();
    m_requests = consumer<PathRequest>().onEvent(
        [this](const sfs::Entity& agent, const PathRequest& request,
               const sfs::Responder<PathRequest>& responder)
        {
          sfs::nav::PlanScheduler::Request job;
          job.owner = agent.getId(); // the scheduler's fair turn
          job.label = "path";
          job.collected = false;     // it answers through the responder
          job.wanted = [responder] { return !responder.superseded(); };
          job.step = [grid = request.grid, responder](auto&)
          {
            // plan on *grid; responder.progress(leg) for each leg, then
            // respond(...) or fail(...)
            return sfs::nav::PlanScheduler::Step::Done;
          };
          m_scheduler->submit(std::move(job));
        });
  }

private:
  sfs::EventConsumer<PathRequest> m_requests;
  // Last: destroyed first, so its workers are joined before anything a job
  // holds goes.
  std::unique_ptr<sfs::nav::PlanScheduler> m_scheduler;
};
```

- Declare the scheduler last, or reset it in the destructor: it's destroyed
  first among the members, so its workers are joined while the rest live.
- `collected = false`: nobody collects a job that answers through its
  responder, so the scheduler forgets it as it ends (see
  [Plan scheduler](../../runtime/navigation/scheduler.md)).
- A plain `sfs::WorkerPool` member works too, for jobs without deadlines or
  fair turns.

## What it can't do, and what to do instead

A service has none of a System's list calls or its registry, so using one
doesn't compile: the compiler says the name isn't declared.

| Written in a service | The compile error says | Instead |
| --- | --- | --- |
| `query<T>()`, `registry->...` | `query`, `registry` not declared | Take what you need from the event, or move the work to a System. |
| `getEntities()`, `registerComponent<T>()` | `getEntities`, `registerComponent` not declared | A System. |
| `observe<T>(...)` | `observe` not declared | A System, or news the system that changes it sends. |
| `m_x = consumer<E>();` | "A Service's consumer needs a handler" | Give it `onEvent()` (or another handler) as it's made. |
| `void update(double) override` | "does not override" | Do the work in a handler, or make it a System. |
| no `create()` | "abstract" | Make its handles in `create()`. |

A service has no timers and doesn't run by itself: work that needs time to
pass is a System's, with the delay as state on a component (a `RespawnTimer`
a System counts down, then sends `RespawnDueEvent`), and the service reacts to
that news.

## When services run

Every step, at its start, after the step's game time is advanced and before
the systems' updates. A service runs only when one of its handles has
something new, and once a step at most.

| Who sent it | A service hears it |
| --- | --- |
| A system, in step k | At the start of step k+1. |
| Another service, added before it | The same run. |
| Another service, added after it | The next step. |
| A game object or the UI | At the next frame's first step. |
| A worker's response | At the next step (responses are taken in at each step's start). |
| A worker's broadcast | At the next frame's first step. |

What a service changes (a component, its scene data, an event it sends)
reaches the systems in the same step. At 16 steps a frame (fast-forward) a
service runs 16 times, so a task it hands out waits one step of game time,
as at normal speed. Paused, or at time scale 0, each frame still has one
step, so services run and requests are answered. Gameplay never depends on
this order: a one-step shift is accepted, never designed around.

Services run on the scene's thread, never in parallel with systems or each
other. Their work off that thread is jobs, which answer through Responders.

## Adding one

- **The editor:** Scene Settings > Services, Add Service. The scene file
  lists it by name: `"services": ["SquadService"]`, beside `"systems"`.
- **Code:** `scene.addService<SquadService>()`.
- **Headless:** `registry.addService<SquadService>()`, and
  `registry.runServices()` before the systems each step.

New > Service in the Assets panel (or Scripts > New Service...) writes a
starting header. Codegen finds services by their base class, as it does
systems.

## Debugging

- The Debug Systems window lists the services after the systems, each with
  its toggle and settings (`settings()`, as a system's).
- Scene Settings > Debug > Events shows a service's consumers under its name,
  with their unread counts and stuck warnings.
- `SFS_CPU_TIMING` times each run as `service/<Type>`; Tracy has a `services`
  zone per step.

## Reference

| Call | Where | |
| --- | --- | --- |
| `create()` | Service | Makes its handles, once. |
| `consumer<E>()` | Service `create()` | A consumer, given a handler, kept as a member. |
| `requester<R>()` | Service `create()` | A requester's builder. |
| `send(e)`, `send(entity, e)`, `cancel<E>(entity)`, `broadcast<E>()` | Service | As a system's. |
| `sceneData<T>()` | Service | The scene's one T, or null. |
| `addSceneData<T>(args...)`, `removeSceneData<T>()` | Service | Adds the scene's T (null outside a registry), or removes it. |
| `createEntity()`, `destroyEntity(entity)` | Service | Makes or destroys an entity (at the next flush). |
| `enabled()`, `setEnabled(on)`, `settings()` | Service | Debug toggling and settings. |
| `addService<T>()`, `forEachService(fn)` | Scene, Registry | Adds one; goes through them. |
| `runServices()` | Registry | Runs those with something new (headless hosts). |

How services work inside the engine:
[Services internals](https://github.com/hurricane-pinecone/Segfault-Simulator/blob/master/engine/docs/core/services/index.md).
