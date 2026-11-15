# Events

An **event** is how one system tells others something: news that something
happened (an enemy took damage, an agent arrived, a worker finished a
path), a command, a request, or an answer. A sender sends a typed event;
consumers pull it, each in its own update, in the registered update order,
like everything else a system reads. There are no callbacks and no immediate
dispatch. This is Bevy's model (`Events<T>`, a cursor per reader), with each
event kept until every consumer has consumed it as in a message broker's
queues, and delivery chosen by the consumer as in brokers (pub/sub readers
and a work queue's takers) and Rx (`switchMap`: the latest per key).

Each registry has its own events: a system uses its [event verbs](#consuming-in-a-system)
(`send`, `read<E>()`, `take<E>()`, ...), and a game object or a scene uses
`scene().events()`.

## Event types

An event type is a plain struct marked `[[sfs::event]]`:

```cpp
// DamageTaken.h
#pragma once
#include <engine/core/ecs/entity.h>

struct [[sfs::event]] DamageTaken
{
  sfs::Entity target;
  float amount = 0.0f;
};
```

Codegen skips a marked type: it is neither a component nor reflected, and it
may be the type named after its header (a game might keep them in
`scripts/events/`). Unmarked, a struct named after its header would be
registered as a component. See [Reflection](../reflection/index.md).

An event holds values only: ids, `sfs::Entity` handles, small vectors or
strings moved in. Never a pointer into a component or a system (it may be read
a frame later) or a `std::function`. A big payload that several readers share
goes in a `std::shared_ptr<const T>`. Don't declare event types in a system's
header: they're shared between the systems that send and read them.
Event types can't be pointers, references, const, or throw while they move:
`send()`, `reader()` and `sender()` all refuse them at compile time.

### What an event is about: its policy

A type may declare what it's about, once, as a `policy`:

```cpp
struct [[sfs::event]] AttackOrder        // about one attacker
{
  static constexpr sfs::EventPolicy policy{.key = sfs::EventKey::Entity};
  sfs::Entity target;
};

struct [[sfs::event]] PathRequest        // about one agent, answered
{
  static constexpr sfs::EventPolicy policy{.key = sfs::EventKey::Entity,
                                           .answered = true};
  glm::vec3 goal{};
};

struct [[sfs::event]] PathLeg { using Request = PathRequest; Leg leg; };
struct [[sfs::event]] PathFound { using Request = PathRequest; bool found; };
```

| Field | Default | Meaning |
| --- | --- | --- |
| `key` | `EventKey::None` | `EventKey::Entity`: each event is about one entity, its key. It's sent with `send(entity, event)`, its rows give the entity, and every keyed send supersedes the key's earlier event for the [latest verbs](#latest-per-key). |
| `answered` | `false` | It's a request that gets replies: askers send it with `request(entity, event)`, its taker answers through a [Reply](#requests-and-answers). Needs `key = EventKey::Entity`. |

A type without a policy is unkeyed and unanswered: plain news. An **answer
type** names its request (`using Request = R;`) and declares no policy: it's
keyed by its request's key, and readers see only answers to a key's current
request. The type says only what it is; **how** an event is delivered (seen
by every reader, or taken by one system) is each consumer's choice, made by
the verb it calls. A verb the type doesn't allow is a compile error:

| Rule | Why |
| --- | --- |
| `answered` needs `key = EventKey::Entity` | answers are routed by key and filtered by the key's current request |
| an answer type declares no policy, and its `Request` is answered | an answer's delivery is fixed by its request |
| `send(event)` takes an unkeyed type, `send(entity, event)` a keyed one, `request(entity, event)` an answered one | the key, and whether answers come back, are the type's |
| `readLatest`, `takeLatest` and `cancel` take keyed types | "latest" is per key |
| an answer type is only read (`read<A>()`, `waiting<A>()`) | its one sender is the request's taker |
| an answered request is taken with `takeLatest<R>()`, not `take` or `takeShared` | its rows carry the Reply it's answered through |

## Sending

```cpp
send(DamageTaken{enemy, 10.0f});               // in a system: its event verbs
send(knight, AttackOrder{enemy});              // keyed: about the knight
request(agent, PathRequest{goal});             // answered: replies come back
cancel<PathRequest>(agent);                    // the agent's request goes
scene().events().send(DamageTaken{enemy, 5.0f}); // in a game object
```

From an update, a lateUpdate, a render, a game object, a hook or while a scene
loads: anything on the thread that owns the events (the one that made the
registry, and after that the one that updates it). A send from any other
thread is logged as an error (once per type, in every build) and dropped:
workers use a [sender](#threads) or a [reply](#requests-and-answers).

A keyed send (or request) starts the key's next **generation**: it supersedes
the key's earlier event for the latest verbs. `cancel<E>(entity)` does the
same with nothing new: the key has no current event. So does destroying the
entity, at the flush.

## Consuming in a system

A system consumes with its event verbs, which it inherits (`sfs::EventVerbs`,
a base that a later event-only class can share). It keeps one cursor per
type it consumes, made on the type's first consuming call: no reader members.

| Verb | Gives | Consumers |
| --- | --- | --- |
| `read<E>()` | every event that arrived since the last update | any number; it consumes nothing, so readers see events a system takes too (damage numbers on screen watching the damage a health system takes) |
| `take<E>()` | every event, as E's one taker | one system |
| `takeShared<E>()`, `takeShared<E>(filter)` | each event to one of the systems that call it, the first in update order; with a filter, only the events it accepts, leaving the rest | several systems, all calling `takeShared` |
| `readLatest<E>()` (keyed) | each key's newest current event | any number |
| `takeLatest<E>()` (keyed) | each key's newest current event, as E's one taker; for an answered type each row carries its `Reply` | one system |

```cpp
void HealthSystem::update(double)         // DamageTaken's one taker
{
  for (const DamageTaken& damage : take<DamageTaken>())
    apply(damage);
}

void DamageNumbers::update(double)        // watches the same damage
{
  for (const DamageTaken& damage : read<DamageTaken>())
    show(damage);
}

void CombatSystem::update(double)
{
  for (auto [attacker, order] : takeLatest<AttackOrder>())  // keyed rows
    attack(attacker, order.target);
}
```

- **Rows**: `const E&` for an unkeyed type, `[entity, event]` for a keyed one
  (which also converts to `const E&`), `[entity, request, reply]` for
  `takeLatest` of an answered type. They point into the queue: nothing is
  copied.
- **One span per update.** The first consuming call of an update fixes the
  span (what arrived since the last update's span); later calls in that
  update give the same events, so helpers can each call it. What's sent after
  it, by this system too, is next update's. `lateUpdate()` is a new span.
- **The first call** registers the system's cursor with the type's queue,
  at the oldest event kept, so a system's first update sees what was sent
  while the scene loaded. From then on the queue keeps every event until the
  system has consumed it.
- **Takers.** A type has one taker (`take`, or `takeLatest`, which the same
  system may both call), or several that all call `takeShared`. A second
  taker, or `take` beside `takeShared`, is logged once as an error and gets
  nothing (and sfs-lsp's `second-consumer` flags it). A sole taker needs no
  per-event state: its cursor says what it took. Shared takers claim each
  event with a 4-byte word, kept beside the events once the type's first
  shared taker appears.
- **Nothing is lost to late consuming.** A system that returns early and
  consumes next update, or is disabled for a while, gets every event when it
  consumes again: holding back is fine (`if (busy) return;` before a take is
  backpressure). Consuming calls belong in `update()` and `lateUpdate()`:
  builds without NDEBUG log one from a hook, a callback or `create()`.
- A removed system's cursors deregister; a disabled one keeps them.
- `hasUnconsumedEvents()` says whether any queue a system consumes holds
  events past its cursor (one or two loads per type).
- Don't send a type while going through that type's rows (the buffer may
  move): collect, then send. Builds without NDEBUG log it and end the loop.
- A system runs its updates through the registry (`Registry::updateSystem()`,
  `lateUpdateSystem()`), which starts each new span; `Scene::update()` does,
  and so does a headless host.

### Latest per key

`readLatest<E>()` and `takeLatest<E>()` give, of the events that arrived
since the last update, those still current for their key: the newest sent,
not cancelled, its entity not destroyed. Of several for one key in a frame,
only the newest is given; one already given isn't given again. Latest-ness is
the consumer's view: `read<E>()` and `take<E>()` of the same type still give
every event.

### Requests and answers

An answered type is a request. The asker sends it with
`request(entity, {...})`; its taker takes it with `takeLatest<R>()` and gets a
`Reply<R>` with each row; askers read the answers with `read<A>()`, which
gives only answers to the key's current request.

```cpp
void MovementSystem::update(double)       // asks for paths
{
  for (auto [agent, leg] : read<PathLeg>())
    follow(agent, leg.leg);               // writes its own state
  for (auto [agent, found] : read<PathFound>())
    settle(agent, found.found);
  ...
  request(agent, PathRequest{goal});      // supersedes the agent's last ask
  if (waiting<PathFound>(agent))          // asked, its answer not in yet
    hold(agent);
}

void PathfindingSystem::update(double)    // PathRequest's taker: it answers
{
  for (auto [agent, asked, reply] : takeLatest<PathRequest>())
    submitSearch(agent, asked, std::move(reply)); // a job keeps the reply
}

// In the job, on a worker:
if (reply.superseded())                   // a newer ask, a cancel, or destroyed
  return Step::Done;
reply.progress(PathLeg{leg});             // more to come
reply.send(PathFound{true});              // its last PathFound
```

- A `Reply` holds the request's key and generation and the request type's
  shared block. `superseded()` is one relaxed load; `progress(answer)` and
  `send(answer)` (its last) return false once it's superseded or the events
  are gone. Any thread: from the owning thread an answer is readable at once,
  from a worker at the next fixed step (`Events::takeIn()`, which
  `Scene::update()` calls between steps) or frame.
- A row's reply borrows the shared block: answering inside the loop costs no
  reference count. Copying or moving it out (into a job) takes a hold.
- `waiting<A>(entity)`: the key has a live current request whose taker
  hasn't sent its last `A` yet. `cancel<R>(entity)` makes it false.
- A newer request supersedes the older at once: its reply's `superseded()`
  turns true, and its answers are dropped as they're sent and again as
  they're taken in, so a late answer to an old request is never read as the
  new one's. Askers hold no ids.
- Each answer type has one sender, the request's taker (sfs-lsp's
  `answer-sent-twice`); a request may have several answer types.
- Answers land in the asker's own state: the asker writes what it needs into
  its own component. A consumer that can't act at once keeps the reply in
  its own work list (scheduler data, not entity state) and drops entries
  whose `superseded()` turns true. `nav::PlanScheduler`'s jobs take a
  `wanted` check (`[reply] { return !reply.superseded(); }`): a job that's no
  longer wanted runs no further step.

## Reading from objects and UI

A game object or a UI reads an unkeyed type through an `EventReader`, a place
in one type's queue. Make it once, in `onCreate`, and keep it as a member;
each `read()` gives what it hasn't read yet, oldest first:

```cpp
class DamageLog : public sfs::GameObject
{
  void onCreate(sfs::Scene& scene) override
  {
    m_damage = scene.events().reader<DamageTaken>();
  }

  void onUpdate(double) override
  {
    for (const DamageTaken& damage : m_damage.read())
      note(damage.target, damage.amount);
  }

  sfs::EventReader<DamageTaken> m_damage;
};
```

- `read()` marks every event it gives read at once: a loop that breaks early
  skips the rest. Its range is one contiguous span, with `size()` and
  `empty()`. Nothing is copied.
- `unread()` counts what `read()` would give, without marking it read;
  `skip()` marks everything kept read.
- A reader made by default reads nothing.
- A reader registers with its type's queue, which keeps every event until
  the reader has read it: a reader that skips frames misses nothing. It
  deregisters as it's destroyed, so it moves and never copies, and one that
  outlives its events reads nothing.
- A reader is its holder's own data (a handle to an engine queue and a place
  in it, about no entity), never a component's. A system doesn't keep one: it
  reads with `read<E>()`.
- Readers read unkeyed types; keyed ones are consumed through the event verbs.
- Don't send a type while going through that type's range: the buffer may
  move. Collect what to send, then send it after the loop. Debug builds log
  an error (once per type) and end the loop; release builds don't check.

## How long an event is kept

Events are kept until they're consumed. Every system cursor and every
`EventReader` registers as a consumer of its type's queue, and the queue keeps
each event until every consumer has passed it.

| Case | Rule |
| --- | --- |
| A type with consumers | Kept until every consumer has consumed it: a reader has read it, a system's span has passed it. Shared takers, as a group, have consumed an event once one claims it; a latest consumer (`readLatest`, `takeLatest`, a read of answers) doesn't hold events superseded for their key. |
| A type with no consumers | Kept for the rest of the frame it was sent in and the whole of the next, then dropped, so a consumer made while a scene sets up still gets what was sent at load. |
| Sent by a worker | Taken in at the next frame boundary (answers also before each fixed step), then kept like the rest. A keyed event gets its generation as it's taken in. |
| Who sees it | Each reader once, in its first update (or `read()`) after the send; a taker takes it once. A system earlier in the update order than the sender sees it the next step or frame: the earlier one reads what the later one wrote last frame. |
| Stepped systems | A Stepped consumer before the writer gets a step's events at its next step; a Frame or Real one gets all the frame's steps' events once, on the last. |
| Order | Within one type, send order. A worker's events keep their own order and arrive at the frame boundary, ahead of that frame's main-thread ones. There's no order across types: what must stay ordered is one type. |
| A consumer that never consumes | Holds its queue. Past 65,536 kept events the queue warns once, naming the consumer that holds the oldest. Past the cap (`Events::setMaxKept()`, 1,048,576 a type by default) the frame boundary drops the oldest with an error naming it, and its next consume counts them as *missed*. |
| A new consumer | Starts at the oldest event kept, so nothing sent while a scene sets up is lost to the order things are made in. |
| Edit mode | An edit-mode scene isn't updated: what's sent there stays until play (or the warning). |

The frame boundary is `Events::update()`. `Scene::update()` calls it each frame
right after the flush, before the systems update; a headless registry's host
calls `registry.events().update()` once a frame, after `flushEntities()`. It
moves each busy queue's oldest kept event up to its slowest consumer and
drops the rest.

**A miss is always logged.** Only the cap drops events a consumer hasn't
consumed. When a consumer's next update (or a reader's next `read()`) finds
such events gone, it starts at the oldest kept, logs a warning naming the
type and how many it missed (once per consumer, in every build, saying so if
the system was disabled), and the count adds to the type's *missed* total in
the debug view. Where an event carries a request or an answer some entity
waits for, a miss would leave it waiting: the warning is how you find it.

### Entities in events

An event names entities by `sfs::Entity`, which may be gone when it is read:
check it like any relationship handle (`if (damage.target)`, a null
`getComponent`). Destroys take effect at the flush, before the frame boundary,
so an entity destroyed in frame N reads invalid from frame N+1, and a reused id
reads invalid by its generation. A keyed event's key is such a handle too; the
flush also supersedes a destroyed key's current event (the latest verbs skip
it, its replies stop, `waiting()` turns false), and a reused id never matches
an old event's generation.

## Threads

The owning thread sends, makes readers and senders, reads and updates. A
worker sends through an `EventSender<T>`:

```cpp
// On the owning thread, as the job is submitted:
sfs::EventSender<BakeDone> done = registry->events().sender<BakeDone>();
pool.submit([done, chunk] { /* ... */ done.send(BakeDone{chunk}); });
```

- A keyed type's sender sends with `send(entity, event)`. Requests are sent
  on the owning thread, and answers go through a [Reply](#requests-and-answers).
- A sender points at its queue's inbox (a mutex, the staged events and a
  count). `send()` locks, appends and returns true; the next
  `Events::update()` swaps the staged events out under the lock and moves them
  in, ahead of that frame's main-thread events, each thread's in its own order.
- Copy a sender into its job once (an atomic increment), not per event.
- A sender outliving its `Events` gets `false` from `send()`: the `Events`
  closes its inboxes as it goes. A sender holds its type's storage, so it
  mustn't outlive the code that defines the type: a job holding one is
  cancelled and joined with its scene.
- A mutex, not a lock-free queue: workers' events are rare, and a mutex keeps
  each sender's order and is simple to check (the tests run under
  ThreadSanitizer, `-DENGINE_CORE_ONLY=ON -DENGINE_TSAN=ON`).

## Events or components

Components hold only an entity's state; everything one system tells another
goes in an event.

| What | Use |
| --- | --- |
| What is so about an entity: a mode, a value, what it carries, what it's waiting for | A component field, written by the system that owns that concern (`observe()` reacts to one appearing). |
| Something that happened | News: an event, `read` by whoever cares. |
| Something to do, where each one counts | A command: an event its one system `take`s (or several `takeShared`). |
| Something to do, where a newer one for the same entity makes the older moot | A keyed event, consumed with `takeLatest` (or watched with `readLatest`). |
| Something to do that the asker must hear back about | A request (`answered`), answered through the taker's `Reply`. |
| A worker's news or result | An event through an `EventSender`, or an answer through a `Reply`. |

Never put a request field, a one-frame flag (`justChanged`, `done`) or a
message field on a component for another system to pick up. Reacting to an
event is the moment; what must outlast the frame (the target an enemy
chases, an agent's path) is state, written by the system that owns it, and
an asker writes an answer's data into its own state.

Events don't replace `observe()` or `registry->componentChanges<T>()`: never
send an event per component write.

## Debugging

These go through the engine's [debug features](../../runtime/rendering/debug-drawing/index.md)
(the editor, Debug builds and development exports):

- `SFS_LOG_EVENTS=DamageTaken,AgentEvent` (names with or without their
  namespaces, or `*` for every type) logs each send's type, policy, number
  and frame (a keyed one's entity and generation), each worker's send with
  its thread, the workers' events as they're taken in, a shared taker's
  claims, cancels, and answers dropped as superseded.
  `debugFeatures::setLogEvents()` sets the list from code; it applies to
  types first used after it.
- A queue past 65,536 kept events warns once, naming the consumer that
  holds them: one that never consumes, a send loop, or a scene nobody
  updates.
- **Scene Settings > Debug > Events** shows, per type: its policy, how it's
  taken, sent last frame, kept, staged from workers, capacity, consumers,
  missed, superseded and dropped answers (`Events::stats()`).
- Profilers see the frame boundary as `events/update`.

In every build: events dropped at the cap and the misses they cause (above),
a second taker, and a call off the owning thread.

## What it costs

| What | Layout | Allocates |
| --- | --- | --- |
| `Events` (in the registry) | queues by event type id, the queues in the order made, the keyed channels, the answer queues, the owner thread's id, the frame count, the kept cap | a queue at a type's first use |
| A queue | one `std::vector<T>` (contiguous), the number of its first event and of the oldest kept (the head), the next number, its consumers (each where it has consumed to, who it is and how it consumes: 24 bytes, a dense list), the inbox pointer, counters (one layout in every build) | its buffer grows to the most it keeps, then keeps its room; a consumer slot at a consumer's first use (freed slots are reused) |
| An inbox | a mutex, two vectors (staged, and the one handed back), an atomic count | once, at the first `sender()` |
| `EventReader<T>` / `EventSender<T>` | a `std::shared_ptr` to the queue's link (which the queue clears as it goes) and its consumer slot: 24 bytes / a `std::shared_ptr`: 16 bytes | never |
| A keyed type's events | beside the events, each one's key (`sfs::Entity`, 24 bytes) and generation (4 bytes), as arrays of their own | with the buffer |
| Claims | 4 bytes per event beside the events, once a type has shared takers | with the buffer |
| A keyed type's channel | a generation word per entity id (4 bytes, atomic), in pages of 1,024 ids made as an id in the page is first sent to, under a fixed 8 KB directory (1M ids); a request type's answer inboxes | the channel at the type's first use, a page per 1,024 ids sent to |
| An answer type's queue | as any keyed type's, plus a word per entity id (paged) for `waiting()` | a page per 1,024 ids answered |
| A system's cursors (`EventVerbs`) | per type it consumes: the queue, its span, an update stamp, its consumer slot and claim number (32 bytes), and a byte per event type id | at a type's first consuming call |
| `Reply<R>` | the channel (a pointer, and a `std::shared_ptr` once held), the key, the generation: 56 bytes | never |

Event type ids come from one table in engine-core keyed by the type's name, as
component ids do (`detail::eventTypeId()`), so the engine, the editor and a
game module agree.

A send is the thread check, the queue's lookup (a cached id, an index) and a
`push_back`; a keyed send also starts the key's generation (its page, a load
and a store) and appends the key and generation. A read is a few loads, then
contiguous iteration. A verb's call looks its cursor up by the type's id (a
byte, an index and a stamp compare), and the latest verbs and answers check
each event's generation against its key's word. Destroying an entity costs
each keyed type a page lookup at the flush. A frame boundary skips an idle
queue after a load or two. A busy one takes in what workers staged, finds
its slowest consumer (a scan of its few consumers; a latest consumer's and
the shared takers' places skip superseded and claimed events) and moves the
head there. When every event is consumed, the common case, it clears the
buffer outright (nothing to do for trivially destructible events); otherwise
it erases the consumed part once the head has passed half the buffer, so
each event is moved at most about once. Once the buffer has grown to the
most it keeps, nothing allocates.

`eventsBenchTests` (Release) measures it. It always prints its results and
gates, and checks the gates only when `SFS_BENCH_GATES` is set, in an
optimised build, so a loaded machine can't fail a test run. Where a plain
vector's own loop costs more than the read target on a machine, that gate is
set against the loop instead:

| Case | Target |
| --- | --- |
| send, a 32-byte trivially copyable event | ≤ 4 ns for an event built beforehand; a `send(T{...})` of a fresh temporary also pays for copying it (about 6 ns), as a plain `push_back` of one does, and stays ≤ 1.5x a `push_back` onto a component's vector |
| read, 1 and 4 readers | ≤ 0.5 ns per event per reader (or a plain vector's loop + 10 %) |
| `update()`, 32 idle queues / a reader having read 10,000 trivially destructible events | ≤ 100 ns / the same as for 10 (+5 %, or + 5 ns: the cache misses on the consumers after 10,000 sends to each of 256 queues) |
| `update()` with a reader that leaves half of each frame's 10,000 events, compacted each frame | ≤ 0.5 ns per event sent |
| `EventSender::send`, 1 thread / 4 contending | ≤ 50 / ≤ 250 ns per event; taking them in ≤ 3 ns per event |
| 1,000 frames of steady traffic after the warm-up, verbs and replies included | no allocation (a static build counts them) |
| keyed send, to many keys / superseding one key's last | ≤ 8 ns |
| `take` / `takeShared` | ≤ 1.5 / ≤ 3 ns an event (a shared take claims, then goes through what it holds) |
| `readLatest` / `takeLatest` | ≤ 4 ns an event |
| `takeLatest` of an answered type, with a reply per row | ≤ 6 ns an event |
| `read` of answers | ≤ 3 ns an answer |
| a verb's lookup, the span already fixed | ≤ 4 ns a call |
