# Events

Events let one system tell others that something happened, like an enemy
dying or a projectile hitting, without either knowing about the other. The
health system says "this enemy died"; the score, the loot and the HUD each
hear it and do their own part.

An event tells *other* systems something. When a system changes an entity's
own state (its health, its target, its path), it writes the component; it
doesn't send itself an event. And work that can't be done now (a long search,
a bake) goes to a worker job (`sfs::WorkerPool`, or the
[plan scheduler](../../runtime/navigation/scheduler.md)), not into an event
queue.

## Declaring an event type

An event type is a plain struct with an attribute that says what kind of
event it is:

```cpp
#pragma once
#include <engine/core/ecs/entity.h>

// News: something happened.
struct [[sfs::event_news]] EnemyDiedEvent
{
  sfs::Entity enemy;
  sfs::Entity killer;
};

// News about one entity: rows give the entity, and a newer one for the
// same entity makes the older one stale.
struct [[sfs::event_entity_news]] AlertedEvent
{
  float level = 0.0f;
};
```

| Attribute | What it is | Sent with |
| --- | --- | --- |
| `[[sfs::event_news]]` | News: something happened | `send(event)` |
| `[[sfs::event_entity_news]]` | News about one entity | `send(entity, event)` |
| `[[sfs::event_request(Success, progress = P, failure = F)]]` | A request about one entity that gets its success response back, or its failure, after any progress ([Requests and responses](#requests-and-responses)) | a requester's `request(entity, request)` |
| `[[sfs::event_response]]` | A response playing a role in a request | a responder's `respond()`, `progress()` or `fail()`, or a request consumer's by entity |

The build reads these attributes (the code generator that also reflects your
components) and tells the compiler what each type is, so mistakes are compile
errors that say how to fix them: sending a type that isn't marked ("isn't an
event: mark it `[[sfs::event_news]]`"), sending news about an entity with
`send(event)`, sending a response yourself, sending a response in the wrong
role, or forgetting to handle a request's success or failure. Declare event
types at namespace scope, one attribute each, in a header or a `.cpp` of
your scripts: the build reads both. Every file of your module sees what each
event type is, so changing an event declaration rebuilds the module.

An event holds values only: ids, entities, numbers, small vectors or strings
moved in. Never a pointer into a component or a system: an event may be read
a frame later.

The examples here end news types in `Event`, requests in `Request` and
responses in `Response`. That's only their style: name yours as you like.

## Sending

A system sends with `send()`:

```cpp
void HealthSystem::update(double)
{
  for (auto [enemy, health] : registry->query<Enemy, Health>())
    if (health.current <= 0 && !health.dead)
    {
      health.dead = true;
      send(EnemyDiedEvent{enemy, health.lastHitBy});
    }
}
```

Sending needs no set-up. News about one entity takes the entity first:
`send(guard, AlertedEvent{0.8f})`. Objects and UI send through the scene:
`scene().events().send(EnemyDiedEvent{...})`.

A send from a thread other than the one that runs the scene is refused (and
logged). Workers send through a [broadcast](#threads).

## Receiving: consumers

A system that wants an event type makes a consumer of it in `create()` and
keeps it as a member. It then reads the events in `update()`:

```cpp
class ScoreSystem : public sfs::System
{
protected:
  void create() override { m_deaths = consumer<EnemyDiedEvent>(); }

  void update(double) override
  {
    for (const EnemyDiedEvent& died : m_deaths.read())
      if (Score* score = died.killer.getComponent<Score>())
        score->points += 100;
  }

private:
  sfs::EventConsumer<EnemyDiedEvent> m_deaths;
};
```

Every consumer sees every event of its type once. Reading takes nothing away
from the others: the score, the loot and the HUD all see each death.

What `read()` gives is a range: loop over it, or hand it to a `std::ranges`
algorithm (`std::ranges::any_of(m_deaths.read(), ...)`).

`read()` gives what arrived since the system's last update. Read as often as
you like within one update: each read gives the same events. Events sent
during the update arrive at the next one. A read in `lateUpdate()` gives
what arrived since the update, and those events then aren't given again: not
to the next update's reads, nor to its handlers.

### Handlers

Instead of reading in `update()`, give the consumer a handler as you make it.
The engine calls it just before the system's `update()`, once for each new
event, and not at all when nothing is new:

```cpp
void LootSystem::create()
{
  m_deaths = consumer<EnemyDiedEvent>().onEvent(
      [this](const EnemyDiedEvent& died) { dropLoot(died.enemy); });
}
```

`onEvents()` gets the update's events at once, for work done once per batch.
A sound system plays one death sound however many enemies died this frame:

```cpp
m_deaths = consumer<EnemyDiedEvent>().onEvents(
    [this](sfs::EventRange<EnemyDiedEvent> deaths)
    { playOnce(m_deathSound, deaths.size()); });
```

A `read()` in `update()` on a consumer with handlers gives the same events
the handlers just got.

### News about one entity

Rows of `[[sfs::event_entity_news]]` give the entity too:

```cpp
for (auto [guard, alert] : m_alerts.read())
  raiseAlarm(guard, alert.level);
```

When only the newest event for each entity matters, use `readLatest()`, or
the handlers `onLatest()` and `onLatestEvents()`. A newer send for the same
entity makes the older one stale, and so do `cancel<E>(entity)` and the
entity's destruction:

```cpp
for (auto [guard, alert] : m_alerts.readLatest())
  guard.getComponent<Guard>()->alertLevel = alert.level;
```

## Event families

News can derive from other news, and then it's sent to the base type's
consumers too. A damage log reads every kind of damage; a burn system reads
only fire:

```cpp
struct [[sfs::event_news]] DamageEvent
{
  sfs::Entity target;
  sfs::Entity source;
  float amount = 0.0f;
};

struct [[sfs::event_news]] FireDamageEvent : DamageEvent
{
  float burnSeconds = 0.0f;
};

// The projectile system sends fire damage:
send(FireDamageEvent{{target, shooter, 5.0f}, 3.0f});
```

Note the inner braces: they set the base's fields.

```cpp
// The damage log sees every DamageEvent and every FireDamageEvent, as a
// DamageEvent, in the order they were sent.
m_damage = consumer<DamageEvent>().onEvent(
    [this](const DamageEvent& hit)
    { log(hit.source, hit.target, hit.amount); });

// The burn system sees only fire.
m_fire = consumer<FireDamageEvent>().onEvent(
    [this](const FireDamageEvent& fire)
    { ignite(fire.target, fire.burnSeconds); });
```

A family is one kind: news derives from news, entity news from entity news.
Requests and responses take no part in families. A send copies the event to
each base type only while that type has consumers, so an unused base costs
next to nothing.

## Requests and responses

A request asks for something about an entity and gets a response back. It
declares its responses by role: the success response it ends with, and,
optionally, a progress response sent along the way and a failure response
it can end with instead:

```cpp
struct [[sfs::event_response]] PathFoundResponse
{
  std::vector<glm::vec3> points;
};

struct [[sfs::event_response]] PathLegResponse
{
  std::vector<glm::vec3> points;
};

struct [[sfs::event_response]] PathFailedResponse
{
  int reason = 0;
};

struct [[sfs::event_request(PathFoundResponse,
                            progress = PathLegResponse,
                            failure = PathFailedResponse)]] PathRequest
{
  glm::vec3 goal;
};
```

A request that can only succeed declares just its success:

```cpp
struct [[sfs::event_response]] DoorOpenedResponse
{
};

struct [[sfs::event_request(DoorOpenedResponse)]] OpenDoorRequest
{
};
```

The first type is the success; `progress =` and `failure =` are optional,
in either order. Each is an `[[sfs::event_response]]` type, and one type
plays one role in a request. It may play roles in other requests too (a
shared "refused" failure), but one system sends a response type, so those
requests are served by the same system; give another server's responses a
type of their own.

The asker makes a *requester* in `create()` and gives it a handler for each
role in one chain:

```cpp
class EnemyAISystem : public sfs::System
{
protected:
  void create() override
  {
    m_paths = requester<PathRequest>()
                  .onProgress([this](const sfs::Entity& enemy,
                                     const PathLegResponse& leg)
                              { startOn(enemy, leg.points); })
                  .onResponse([this](const sfs::Entity& enemy,
                                     const PathFoundResponse& found)
                              { follow(enemy, found.points); })
                  .onFailure([this](const sfs::Entity& enemy,
                                    const PathFailedResponse&)
                             { wander(enemy); });
  }

  void update(double) override
  {
    for (auto [enemy, chase] : registry->query<Enemy, Chase>())
      if (chase.targetMoved && !m_paths.waiting(enemy))
        m_paths.request(enemy, PathRequest{chase.targetPosition});
  }

private:
  sfs::EventRequester<PathRequest> m_paths;
};
```

The compiler checks the chain: the success must be handled, and so must a
declared failure, unless you skip it on purpose with `.ignoreFailure()`.
Leaving one out is an error that names the request. Progress is optional: a
requester without `.onProgress()` simply gets none. `onResponses()`,
`onProgresses()` and `onFailures()` take the update's responses of a role
at once (an `sfs::ResponseRange<R, Response>`). The handlers run just before
the system's `update()`.

A request belongs to the requester that made it. Each requester has its own
current request per entity, and gets only the responses to it: if the AI and
the camera both ask for a path to the same enemy, each gets its own path, and
a newer request from one doesn't replace the other's.

| Requester | |
| --- | --- |
| `request(entity, r)` | Asks about the entity. A newer request from this requester for the same entity replaces its older one: the older one's responses are dropped. |
| `waiting(entity)` | Whether this requester's current request about the entity has no final response (success or failure) yet. |
| `cancel(entity)` | Drops this requester's current request about the entity: its responses are dropped and `waiting()` turns false. |

Responses of one role arrive in the order they were sent, and a request's
progress always arrives before its final response, whatever order the chain
gives the handlers: when a leg and the whole path arrive in one update, the
enemy starts on the leg and then follows the path.

### Serving a request

Whoever serves the request consumes it like any event. Each row carries a
`sfs::Responder`:

```cpp
void PathfindingSystem::update(double)
{
  for (auto [enemy, request, responder] : m_requests.readLatest())
  {
    if (std::optional<Path> path = findPath(enemy, request.goal))
      responder.respond(PathFoundResponse{path->points});
    else
      responder.fail(PathFailedResponse{1});
  }
}
```

| Responder | |
| --- | --- |
| `respond(success)` | The success response: final, so the request closes and the asker's `waiting()` turns false. |
| `progress(p)` | A progress response, with more to come (a long path's first leg), if the request declares one. |
| `fail(failure)` | The failure response: final, like `respond()`, if the request declares one. |
| `superseded()` | Whether a newer request from the same requester, a cancel, the entity's destruction or the requester's has replaced this one: stop working on it. A request already answered by entity before the server read it isn't superseded, but responding to it is refused. |
| `key()` | The entity the request is about. |

Each takes only its role's type: anything else is a compile error. The
first final response taken in wins, and a later one is refused. Copy a
responder into a job and respond from a worker thread: the response reaches
the asker at the next frame step. Until then the request stays open (its
`respond()` returned true, but `waiting()` and `open()` are still true), so
a final sent on the scene's thread in the meantime wins and the worker's is
dropped. Requests from different requesters about the same
entity each get their own row and responder; use `readLatest()` to skip
requests already replaced by newer ones from the same requester.

A request handler gets the same three things:

```cpp
m_requests = consumer<PathRequest>().onEvent(
    [this](const sfs::Entity& enemy, const PathRequest& request,
           const sfs::Responder<PathRequest>& responder)
    { responder.respond(PathFoundResponse{plan(enemy, request.goal)}); });
```

Take the responder as `const sfs::Responder<R>&` when you respond at once.
Taking it by value, or keeping a copy for a job, costs a reference count.

### Tell me once you're finished

Some requests are answered much later: "tell me once the door is open",
"tell me when the crafting's done". The server doesn't need to keep anything
for them. It notes what to do on its own components when the request comes
in, and answers later by the entity, through its consumer:

```cpp
void DoorSystem::create()
{
  m_opens = consumer<OpenDoorRequest>().onEvent(
      [this](const sfs::Entity& door, const OpenDoorRequest&,
             const sfs::Responder<OpenDoorRequest>&)
      { door.getComponent<Door>()->opening = true; });
}

void DoorSystem::update(double dt)
{
  for (auto [door, state] : registry->query<Door>())
    if (state.opening && (state.angle += state.speed * dt) >= state.openAngle)
    {
      state.opening = false;
      m_opens.respond(door, DoorOpenedResponse{});
    }
}
```

| Consumer of a request type | |
| --- | --- |
| `respond(entity, success)` | Sends the success response to every requester whose request about the entity is still open, each hearing only its own, and closes them. |
| `progress(entity, p)` | Sends a progress response to every open request about the entity. |
| `fail(entity, failure)` | Sends the failure response to every open request about the entity, and closes them. |
| `open(entity)` | Whether any requester's request about the entity is still open. |

A request that was replaced, cancelled, or whose entity or requester has
gone isn't reached, and with nothing open these return false. They run on
the scene's thread only: a job responds through a `Responder`. A request
answered by entity and by a `Responder` both is fine: the first final
response taken in wins. Each request gets its own copy of the response, so
a response type answered by entity must be copyable.

Responses are events too: another system may `consumer<PathFoundResponse>()`
to watch every response sent, whoever asked, with rows `[entity, response]`.
Each response watched is copied to its watchers, so a type that can't be
copied can't be watched. A response can't be sent with `send()`, only
through a responder or a request's consumer.

## Objects and UI

A game object makes its consumers and requesters in `onCreate`, through the
scene, and keeps them as members. Their handlers run just before its
`onUpdate`. A HUD kill feed:

```cpp
class KillFeed : public sfs::GameObject
{
protected:
  void onCreate(sfs::Scene& scene) override
  {
    m_deaths = scene.events().consumer<EnemyDiedEvent>().onEvent(
        [this](const EnemyDiedEvent& died)
        { addLine(died.killer, died.enemy); });
  }

private:
  sfs::EventConsumer<EnemyDiedEvent> m_deaths;
};
```

A consumer made anywhere else has no owner: its handlers never run, and each
`read()` gives what arrived since the last.

## How long an event is kept

Each event is kept until every consumer of its type has passed it, so a
consumer that reads late, or a system that's disabled for a while, misses
nothing. A type with no consumers keeps an event for the rest of the frame
it was sent in and the whole of the next. A consumer made in `create()` sees
everything sent from then on, even events sent while the scene loads.

A consumer that never reads holds its queue. Past 65,536 kept events the
queue warns, naming the consumer, and past `Events::setMaxKept()` (1,048,576
a type by default) it drops the oldest with an error; the consumer's next
read logs how many it missed.

| Who sees an event | When |
| --- | --- |
| A system after the sender in the update order | The same frame (or step). |
| A system before the sender | The next frame or step. |
| A Stepped system before the sender | At its next step. A Frame system gets all the frame's steps' events once. |
| A worker's broadcast or response | At the next frame boundary (responses also before each fixed step). |

Within one type, events arrive in send order. There's no order across types:
what must stay ordered belongs in one type.

### Entities in events

An entity in an event may be gone by the time it's read: check it like any
handle (`if (died.killer)`). The destruction of the entity a request or
entity news is about drops its current one (its responder reports
`superseded()`, `waiting()` turns false). A send through a handle to a
destroyed entity is refused, with a warning, even once a new entity has its
id.

## Threads

The scene's thread sends, makes consumers and reads. A worker job sends news
through a broadcast, taken on the scene's thread and copied into the job:

```cpp
sfs::EventBroadcast<LevelBakedEvent> broadcast =
    registry->events().broadcast<LevelBakedEvent>();
pool.submit([broadcast, level] {
  bake(level);
  broadcast.send(LevelBakedEvent{level});
});
```

Broadcast events arrive at the next frame boundary, in each thread's order.
A worker responds to a request through a copied responder. Don't let a job
holding either outlive its scene.

## Events or components

| What | Use |
| --- | --- |
| What's so about an entity: its health, its target, its path | A component, written by the system that owns it. |
| Something happened that others care about | News. |
| Something happened to one entity, where only the newest matters | Entity news, read with `readLatest()`. |
| Something to ask for and hear back about | A request with its responses. |
| A worker's result | A broadcast, or a response through a responder. |

Never put a flag (`justDied`), a message or a request field on a component
for another system to pick up: send an event. And never send an event to
yourself to change your own entity's state: write the component.

## Things to watch out for

- Make consumers and requesters in `create()` (or `onCreate`) and keep them
  as members. One made in `update()` is refused and logged; one made as a
  local is gone at the end of the call. The `event-handle-outside-create`
  lint flags both.
- Sending while you go through events is fine, even the same type from its
  own handler (splash damage sending more damage, a chain of explosions).
  What you send then arrives at the next update, never in the loop that sent
  it.
- Give handlers as the consumer is made. One given while handlers run is
  refused.
- A handler that disables its system, or destroys its object, stops that
  update: the update itself doesn't run.
- `skip()` passes every event kept, this update's included.
- `cancel<E>(entity)` cancels the entity's current `E`. The copy a derived
  event left in its base's queue stays current there: cancel the base type
  too if its consumers shouldn't see it.
- A consumer that reads only sometimes still holds every event until it does.
- Use a range (what `read()` gives, or an `onEvents` handler's parameter) in
  the loop or handler that gets it. A range kept past its frame holds its
  type back from every consumer until it goes, with a warning.
- A request's responses come only for its current request: after a newer
  request or a cancel, the older one's late responses are dropped.

## Debugging

In development builds:

- `SFS_LOG_EVENTS=EnemyDiedEvent,PathRequest` (or `*`) logs each send with
  its kind and frame, workers' sends, cancels, and responses dropped as
  superseded.
- **Scene Settings > Debug > Events** lists each type's kind, what it sent
  last frame, what it keeps, its consumers and their unread counts, and
  missed, superseded and dropped events.

## Reference

| Call | Where | |
| --- | --- | --- |
| `send(e)`, `send(entity, e)` | System; `scene().events()` | Sends news, entity news. |
| `consumer<E>()` | System `create()`; `scene().events()` in `onCreate` | A consumer, kept as a member. |
| `requester<R>()` | System `create()`; `scene().events()` in `onCreate` | A requester's builder. |
| `broadcast<E>()` | System; `scene().events()` | A broadcast for a worker. |
| `read()`, `readLatest()` | `EventConsumer` | What arrived since the last update; the newest per entity. |
| `onEvent(fn)`, `onEvents(fn)`, `onLatest(fn)`, `onLatestEvents(fn)` | `EventConsumer` | Handlers, run before the owner's update. |
| `unread()`, `skip()` | `EventConsumer` | How many it hasn't read; pass them all. |
| `onResponse(fn)`, `onProgress(fn)`, `onFailure(fn)`, `onResponses(fn)`, `onProgresses(fn)`, `onFailures(fn)`, `ignoreFailure()` | requester builder | Handlers by role: the success required, a declared failure handled or ignored, progress optional. |
| `request()`, `waiting()`, `cancel()` | `EventRequester` | See above. |
| `respond()`, `progress()`, `fail()`, `superseded()`, `key()` | `Responder` | See above. |
| `respond(entity, s)`, `progress(entity, p)`, `fail(entity, f)`, `open(entity)` | `EventConsumer` of a request type | Answers every open request about an entity later, by entity. |
| `cancel<E>(entity)` | `Events` | Makes an entity's current entity news stale. |
| `setMaxKept(n)` | `Events` | The most events a type keeps. |

How events work inside the engine: [Events internals](../../../engine/docs/core/events/index.md).
