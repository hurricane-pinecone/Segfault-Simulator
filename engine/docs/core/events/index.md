# Events internals

How the events system works inside the engine. What it's for and how a game
uses it: [Events](../../../../docs/core/events/index.md).

The code is in `engine/include/engine/core/events/` and
`engine/src/core/events/`.

## Declarations and the traits header

Event types are declared by attributes (`[[sfs::event_news]]`,
`[[sfs::event_entity_news]]`, `[[sfs::event_request(Success, progress = P,
failure = F)]]`,
`[[sfs::event_response]]`). Compilers ignore them (`-Wno-attributes=sfs::`);
sfs-codegen reads them (`parser.cpp`: `typeMarks()`, `parseEvent()`, the base
clause through `basesOf()`) and writes a header of
`sfs::detail::EventTraits` specializations (`generator.cpp`:
`generateEventTraits()`):

```cpp
namespace { struct FireDamageEvent; }

namespace sfs::detail
{
template <>
struct EventTraits<::FireDamageEvent>
{
  static constexpr EventKind kind = EventKind::News;
  using Ancestors = TypeList<::DamageEvent>;
};
}
```

- The primary template (`eventTraits.h`) says "not an event". The header
  forward-declares each type in its namespaces (an unnamed namespace too, so
  a test's anonymous types work) and names types from the global scope.
- A request's roles, from `[[sfs::event_request(Success, progress = P,
  failure = F)]]`: `Success`, `Progress` and `Failure` (void when not
  declared), which every check reads; other kinds have none, and
  `detail::RoleType` is void for them. Each is resolved to the qualified
  name of a scanned type; a name not among them is written as given,
  qualified by the request's namespaces, for the compiler to resolve (an
  engine type). Codegen rejects a missing success, an unknown or repeated
  role, and one type in two roles.
- `Ancestors` is every event-marked base, transitively, nearest first,
  resolved within the scanned files.
- Codegen's `event-declaration` errors: two event attributes on a type, one
  on a union or on a field of a type codegen reads, an event nested in a
  class, function or template or unnamed, an event in an inline namespace
  (its forward declaration would declare another type), a request listing a
  scanned type that isn't a response, a family member of another kind, a
  news type reaching one base twice other than as a virtual base (the
  fan-out's cast would be ambiguous), a cycle of event bases, a request or
  response in a family, the same qualified name declared twice
  differently. tree-sitter's grammar can't read an attribute
  on an enum, so an enum isn't checked.
- Names are looked up as C++ would (`codegen::lookUpName()`, shared with
  sfs-lsp): through the enclosing namespaces, namespace aliases,
  using-declarations, type aliases and typedefs (up to eight deep) and
  using-directives, each found type written by its real qualified name. A
  response named through a using name or alias that no scanned header
  declares is an `event-declaration` error (write its full name).
- Limits of resolving within the target: a game news type deriving from an
  engine news type gets no ancestors (no fan-out and no kind check); an
  undeclared qualified response is written from the global namespace, and
  an undeclared unqualified one with no using names is qualified by the
  request's namespaces (write `::Name` for the global one); using names in
  included headers aren't seen.
- Where it goes: `sfs_generate_reflection()` (game modules, web games) writes
  `<target>EventTraits.gen.h` beside the registration and force-includes it
  into every file of the target (`-include`, `/FI`), the editor's UI preview
  module included; `sfs_event_traits()` does the same from any sources (the
  engine's event tests). The engine's own event types' header is written to
  `<build>/engine/generated/include/engine/generated/engineEventTraits.gen.h`,
  included by `eventTraits.h` (`__has_include`) and installed with the SDK.
  `sfs_generate_reflection(engine-core ...)` in engine/CMakeLists scans the
  component headers (`HEADERS`) and every other header under
  `include/engine/` (a glob, `EVENT_SOURCES`, scanned for events alone), so an
  engine event type is seen wherever it's declared (such as
  `runtime/navigation/agentEvent.h`) with no list to keep. Any engine header
  edited reruns that scan (about half a second); the traits header is
  rewritten only when an event changes. It only forward-declares each type,
  so core never includes a runtime header. A game module's scan
  covers its headers and, through `--event-source`, its .cpp files.
  Configuring writes each target's header if it's missing, the engine's own
  included (the real one when a codegen executable is at hand, as in a cross
  build or against an installed engine, else one with no event types) and
  removes its stamp, so a language server compiles the files before the first
  build and the build's codegen always runs once to write the real one. Each
  command's output is a stamp it touches every run; the headers and the
  registration it writes only when they change are byproducts. So an edited
  source that changes no declaration runs codegen once, recompiles nothing
  else, and doesn't run it again (`codegenBuildTests` checks both).
  Configuring also removes the stamp when a file the command writes is
  missing, so a generated file deleted by hand comes back after a
  reconfigure (without one, the build stops on the missing file). The UI
  preview module takes the module's header in its precompiled headers
  rather than forced ahead of them, which would keep GCC from using the
  precompiled header.
  A core-only build has no codegen; given `SFS_CODEGEN_EXECUTABLE` (a native
  build's `bin/sfs-codegen`) it uses that for the traits, and without one it
  leaves the event tests out.
- `eventRules.h` reads the traits: `kKind`, `kKeyed` (entity news, requests,
  responses), `kOwnChannel` (entity news, requests: each send starts a
  generation), `kRequest`, `kResponse`, and `checkEvent<T>()`, which every
  send, consumer, requester, responder and queue instantiates. Its failures
  name the types in the failing template's arguments (`NotAnEvent<T>`,
  `NotAResponse<R, A>`, `AncestorOfAnotherKind<E, B>`, `IsAResponse<T>`),
  since a C++23 static_assert message can't be built from them.

## Queues

`detail::EventQueue<T>` holds one type's events in one `std::vector<T>`
(contiguous), with, for a keyed type, each event's key (`sfs::Entity`) and
stamp (a generation) as arrays of their own. Events are numbered: the
buffer's first (`m_baseId`), the oldest kept (`m_oldestId`, the head) and the
next (`m_endId`). Consumers are slots in the queue (`ConsumerSlot`): where it
has read to, its span, its owner, its name, how it reads (read or
readLatest), and a latest scan cache.

- **Retention**: the frame boundary (`advance()`) moves the head to the
  slowest consumer's place (a latest consumer's place skips superseded
  events) and drops the rest. A type with no consumers keeps an event until
  the second boundary after it was sent.
- **Compaction**: when every event is read, the buffer is cleared outright;
  otherwise the read part is erased once the head has passed half the buffer,
  so each event moves at most about once. Nothing allocates once the buffer
  has grown to the most it keeps.
- **The cap**: past 65,536 kept events the queue warns once (every build),
  naming the holder; past `setMaxKept()` the oldest are dropped with an error
  and counted as the holder's missed events, logged at its next span.
- **Families**: a queue holds pointers to its ancestors' queues
  (`m_ancestors`, linked as the queue is made). `push()` appends, then copies
  the event as each base to each base queue that has consumers (`fanOut()`);
  a keyed copy takes a generation from the base's own channel. Workers'
  events fan out as they're taken in. The copies append without fanning out
  again: the ancestor list is already transitive. `cancel<E>()` supersedes
  only E's own channel: a copy in a base queue stays current there.
- **Reading never moves the buffer**: rows point into the queue's vectors,
  so while any range of a queue is alive (`EventRange` counts itself in
  `beginRead()`/`endRead()`, copies included; a handler's range lives for its
  loop) a send to that queue, its own type or a derived type's copy, goes to
  a pending lane (one branch per send). When the last range ends, the lane
  joins the buffer, numbered after it in send order (`foldPending()`), so
  those events belong to the next span. The frame boundary and the workers'
  take-in leave a queue alone while a range is alive. A range kept past its
  frame is misuse: the boundary warns once per queue (every build), the
  lane counts in `stats().kept`, warns past 65,536 and is capped at
  `setMaxKept()` (its oldest dropped and counted as missed). A range holds a
  raw pointer to its queue: one kept past its `Events` writes freed memory
  as it goes (consumers, through their link, don't).

## Keyed channels

Entity news and requests have a `KeyedChannel`: a generation word per entity
id (31 bits and a live bit, atomic), in pages of 1,024 ids under a fixed
directory. A send starts the key's next generation; `current(id, stamp)`
tells whether an event is still its key's newest. The flush calls
`Events::entityRemoved()`, which supersedes a destroyed key's current event
and records the least generation a living entity with that id can have, so
a send through a stale handle is refused (`refuseStale()`).

A request belongs to its requester. A request type's channel holds its
`RequestTickets`: one ticket (32 bytes) per (requester, entity) asked about,
in blocks of 256 that never move, under a fixed directory (1M tickets per
type). A ticket holds its request's generation as an atomic word, readable
by a responder on any thread; its requester, entity and responded
generation, and its links, are the owning thread's. Each request takes the
type's next generation (unique to the type, 31 bits), so a row whose ticket
was freed and reused never matches. A requester is a number on the type
(`addRequester()`, freed numbers reused, as their tickets went with them).
Rows of requests and responses carry their ticket beside the key and stamp.

- An entity's tickets are a doubly linked, counted list from its id (a page
  of heads per 1,024 ids asked about), so the flush walks only the
  destroyed entity's: a page lookup and its tickets, whatever the number of
  requesters. Its open tickets come first: one goes to the back as its
  final response is taken in (`responded()`), and to the front as it's
  asked again, so `open()` looks at the first and an answer by entity stops
  at the first closed one.
- A requester's tickets are a doubly linked, counted list from its number,
  so its going frees only its own: their words change, so their rows stop
  being current and their responders report `superseded()`. Unlinking from
  either list is constant time, so destroying requesters is linear.
- A request, `waiting()` and a cancel find the ticket through the shorter of
  the entity's and the requester's lists: constant time for a requester
  asking about one entity, or an entity asked about by one requester, however
  many others there are; only many requesters each asking about many of the
  same entities walk further. A cancel frees the ticket; a newer request
  reuses it with a new generation.
- Responses are routed as they're kept: a response queue keeps, per
  requester that handles it, the numbers of that requester's responses, so
  a requester's handlers go through its own alone (`ResponseRange`), however
  many requesters there are. Waking is per queue: any response of the type
  runs every requester's handler for it, which finds its route empty (about
  33 ns a requester in a frame with any response of the type). A route is
  bounded by the queue's cap: when the cap drops responses, their routed
  numbers go too (and a pending-lane drop moves the later ones down), and a
  route past 65,536 unread warns, naming its requester's owner.
- Memory follows the requests outstanding: about 32 bytes each, plus the
  blocks' rounding and the entity pages. `EventStats::requestBytes` reports
  it (the bench: 1,000 requesters asking about 20 entities each).
- `Events` has no request, cancel or waiting of its own for requests: they go
  through a requester.

## Requests, responses and responders

- Responses to request R of type A live in their own queue, of
  `detail::ResponseTo<R, A>` (which derives from A and names R), keyed by the
  request's key, stamped with its generation and naming its ticket. A
  requester's handlers read its own routed rows of that queue
  (`ResponseRange`, each checked for a current ticket of its own number), so
  one response type can answer several requests and each requester sees
  only its own. Response queues are dispatched type by type, in the order
  the handlers were given.
- A response kept is also copied to A's own queue while A has consumers
  (`watch()`), keyed by the request's key: observers with `consumer<A>()`.
  A's own queue has no channel (`kOwnChannel` is false).
- A request's final response marks its ticket as responded, which
  `waiting()` reads.
- `Responder<R>` is the request type's channel (an aliasing
  `std::shared_ptr`: a row's responder borrows it with no reference count, a
  copy holds it), the key, the generation and the ticket. The channel lives
  as long as the events, so a borrowed responder stays safe when its
  requester goes during the loop. A request handler is
  called with the row's responder as an lvalue, so `const Responder<R>&`
  keeps the borrow; by value it copies (a reference count). On the owning
  thread it appends straight to the response queue; from a worker, to the
  response inbox under its lock, with the last response's bit in the stamp
  and the ticket. It's superseded once the channel's owner is 0 (the events
  are gone) or its ticket no longer holds its generation.
- The requester builder (`EventRequesterBuilder<R, kSuccess, kProgress,
  kFailure>`) carries the roles handled so far in its type. Each step
  (`onResponse`/`onProgress`/`onFailure`, their batch forms,
  `ignoreFailure()`) checks the role is declared and not handled yet
  (`NoProgressToHandle<R>`, `NoFailureToHandle<R>`, `RoleHandledTwice<R>`)
  and keeps its handler by role; the conversion to `EventRequester<R>`
  checks the success is handled and a declared failure handled or ignored
  (`SuccessNotHandled<R>`, `FailureNotHandled<R>`), then gives the owner the
  handlers in role order (progress, success, failure), so a request's
  progress runs before its final in an update. A
  role not handled opens no route, so its responses aren't noted for that
  requester.
- Server sends are checked per role (`checkRoleSent()`): `respond()` the
  success, `progress()` the progress, `fail()` the failure
  (`NotTheSuccessResponse<R, T>` and the like). A success or failure is
  final: it marks the ticket responded (closed), and the response queue
  refuses anything more for a closed request, so the first final wins.
- Answering by entity: a request type's consumer's `respond(entity, s)`,
  `progress(entity, p)`, `fail(entity, f)` and `open(entity)` walk the
  entity's open tickets (`RequestTickets::forEachOpen()`) and respond to
  each as a Responder would on the owning thread, with a copy of the
  response each (a move-only response type is a compile error). Nothing but
  the ticket is kept: the request's payload went with its row. Owning
  thread only: off it they log once per type, pointing a job at its
  Responder, and do nothing.
- A worker's final is taken in at the next step, so until then the request
  is open: `waiting()` and `open()` are true, and a final sent on the owning
  thread first wins (the worker's is dropped as it's taken in). A request
  answered by entity before its row is read isn't superseded, but its
  responder's sends are refused.

## Owners and handlers

A system and a service (both through `EventVerbs`) and a game object each
hold a `detail::EventOwner`: an update stamp, the consumers it made, and
their handlers (`std::function<void(EventQueueBase&, uint32_t)>`, made once
in `create()`).

- `Registry::updateSystem()` and the scene's object loop run
  `begin()` (a new stamp), `dispatch()` (each handler whose consumer has
  something new), the update (unless a handler disabled the system or
  destroyed the object), then `end()`. `lateUpdateSystem()` runs `begin()`
  and `end()` around `lateUpdate()` too, so a read there fixes a span of its
  own, which the next update's handlers then don't get. A consumer's span is
  fixed by the first call in the update (a handler or a read), so a read
  after the handlers gives the same span; outside an update each call is a
  span of its own (and logged in builds without NDEBUG). `skip()` passes the
  fixed span too.
- A handle is `{link, slot, owner}`: the queue's link (a `shared_ptr` the
  queue clears as it goes, so a handle outliving its events reads nothing)
  and its slot. Moving it is a copy of the three; destroying it frees the
  slot and tells the owner the slot names (`forget()`). An owner going
  clears its slots' owner, so a handle moved into longer-lived storage goes
  safely.
- Each slot has a generation, raised as it's given out, which the owner
  keeps with each consumer and handler. One whose slot was freed or given to
  another consumer since is skipped, and the owner clears them out once
  they're half its consumers (after `dispatch()`, if it's running), so a
  system letting go of thousands of requesters costs each the same. A gone
  consumer's handler, and what its lambda holds, is destroyed at that clear
  rather than at once: finding it sooner would cost a walk of the owner's
  handlers per consumer. It never runs.
- `dispatch()` runs handlers by index, and a handler given during dispatch
  is refused (it would move the running one).
- `Registry::runServices()` hosts the services: for each enabled one whose
  handles have something new (`hasUnconsumed()`, one compare per handle),
  `begin()`, `dispatch()`, `end()`, with no update between. See
  [Services internals](../services/index.md).
- An object's handles get their owner through `Events::m_settingUp`, which
  `Scene::create()` sets around `onCreate`.

## Threads

Workers stage events in a queue's inbox (`EventInbox<T>`: a mutex, two
vectors, an atomic count), through an `EventBroadcast<T>` or a responder.
`Events::update()` (and `takeIn()` before each fixed step) swaps the staged
events out under the lock and appends them, ahead of that frame's
main-thread events; a keyed worker event gets its generation there.
Responses whose queue doesn't exist yet are adopted (the queue is made) at
the next take-in. The tests run under ThreadSanitizer.

## Layout

| What | Layout | Allocates |
| --- | --- | --- |
| `Events` | queues by event type id, the queues in the order made, the keyed channels, the response queues, the owner thread's id, the frame count, the kept cap | a queue at a type's first use |
| A queue | the buffer (and key and stamp arrays for a keyed type), the three numbers, the consumer slots, the inbox pointer, counters, the ancestor pointers | its buffer grows to the most it keeps; a slot per consumer (reused) |
| `EventConsumer<E>` | the queue's link, its owner, its slot: 32 bytes | never |
| `EventRequester<R>` | the events, its owner, its owner's type, the request type's channel and its number there, its response consumers | a consumer per response type handled, in `create()` |
| A request type's tickets | 32 bytes per outstanding request in blocks of 256, a fixed 32 KB directory, a page of entity heads (12 bytes an id) per 1,024 ids asked about, 12 bytes per requester (its list's head), 4 per freed number, and a route (about 56 bytes and its ids) per response type it handles | a block per 256 tickets at once, a page per 1,024 ids |
| `Responder<R>` | the request type's channel (aliasing `shared_ptr`), the key, the generation, the ticket | never |
| A keyed channel | a word per entity id in pages of 1,024 under an 8 KB directory; a request type's response inboxes | a page per 1,024 ids sent to |
| An owner | its stamp and flags, its consumers, its handlers | in `create()` |

Event type ids come from one table in engine-core keyed by the type's name
(`detail::eventTypeId()`), as component ids do, so the engine, the editor and
a game module agree.

## Benchmarks

`eventsBenchTests` (Release) prints its results and gates, and checks the
gates only when `SFS_BENCH_GATES` is set in an optimised build.

| Case | Gate |
| --- | --- |
| send, a 32-byte event built beforehand | ≤ 4 ns; a fresh temporary ≤ 1.5x a `push_back` onto a component's vector |
| read, 1 and 4 consumers | ≤ 0.5 ns per event per consumer (or a plain loop + 10 %) |
| `update()`, 32 idle queues / 10,000 events read | ≤ 100 ns / the cost for 10 + 5 % (or + 5 ns) |
| `update()` compacting behind a slow consumer | ≤ 0.5 ns per event sent |
| `EventBroadcast::send`, 1 thread / 4 contending | ≤ 50 / ≤ 250 ns; taking in ≤ 3 ns an event |
| keyed send, many keys / superseding one key | ≤ 9 ns (about 7 measured; the design's 8 with headroom) |
| a requester's request (its ticket through the shorter of its and the entity's lists, then the append with the ticket) | ≤ 18 ns (measured about 14.5, above a keyed send's 8 ns target: the list's head and the ticket's block are dependent loads, then the fourth lane; with headroom for a loaded machine) |
| memory, 1,000 requesters asking about 20 entities each | ≤ 64 bytes per outstanding request |
| the flush of 10,000 destroyed entities with 1,000 requesters | ≤ 1.3x the flush with none + 20 ns an entity |
| a request, `waiting()` and a cancel with 10,000 requesters on one entity | ≤ 2x with 10 + 5 ns |
| a requester going, of 10,000, each with a handler | ≤ 2x of 1,000 + 20 ns (its owner clears consumers that went in batches) |
| a requester handling a response, 4,000 requesters | ≤ 2x with 100 + 20 ns |
| `onEvent` / `onEvents` handler | ≤ a read loop + 1.5 ns / + 0.5 ns an event |
| `readLatest` | ≤ 4 ns an event |
| reading a requester's requests and responding to each | ≤ 24 ns an event (about 19 measured, above the design's 6 ns for reading with responders: each response is four appends, a ticket check, the responded field and the route) |
| answering 10,000 open requests later by entity, one each | ≤ 21 ns a response (about 17 measured, the asker hearing its responses each round: the key's stale check and its open tickets, then a response as a Responder's) |
| a requester's `onResponse` | ≤ 4 ns a response |
| a worker's response | ≤ 60 ns |
| a send with one consumed event base | ≤ 2.5x the same send with no consumer of the base (two appends and a check) |
| 1,000 frames of steady traffic after warm-up | no allocation (a static build counts) |
