# Events

An **event** is how one system tells others something: news that something
happened (a cell of a field changed, an agent arrived, a worker finished a
plan), a notice, a request, or an answer. A sender sends a typed event; every
reader of that type reads it once, in its own update. There are no callbacks
and no immediate dispatch: readers pull, in the registered update order, like
everything else a system reads. This is Bevy's model (`Events<T>`,
`EventReader`, `EventWriter`, two buffers and a cursor per reader).

Each registry has its own events: `registry->events()` in a system,
`scene().events()` in a game object or a scene.

## Event types

An event type is a plain struct marked `[[sfs::event]]`:

```cpp
// FieldCellChanged.h
#pragma once
#include <glm/glm/ext/vector_int2.hpp>

struct [[sfs::event]] FieldCellChanged
{
  glm::ivec2 cell;
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

## Sending

```cpp
registry->events().send(FieldCellChanged{cell});
```

From an update, a lateUpdate, a render, a game object, a hook or while a scene
loads: anything on the thread that owns the events (the one that made the
registry, and after that the one that updates it). A send from any other
thread is logged as an error (once per type, in every build) and dropped:
workers use a [sender](#threads).

## Reading

A reader is a place in one type's queue. Make it once, in `create()`, and keep
it as a member; each `read()` gives what it hasn't read yet, oldest first:

```cpp
class CropSystem : public sfs::System
{
protected:
  void create() override
  {
    m_changed = registry->events().reader<FieldCellChanged>();
  }

  void update(double) override
  {
    for (const FieldCellChanged& changed : m_changed.read())
      regrow(changed.cell);
  }

private:
  sfs::EventReader<FieldCellChanged> m_changed;
};
```

- `read()` marks every event it gives read at once: a loop that breaks early
  skips the rest. Its range is two spans (the last frame's unread tail, then
  this frame's), gone through as one, with `size()` and `empty()`. Nothing is
  copied.
- `unread()` counts what `read()` would give, without marking it read;
  `skip()` marks everything kept read.
- A reader made by default reads nothing.
- A reader is the reading system's own data (a handle to an engine queue and
  a place in it, about no entity), never a component's. Objects and UI keep
  one as a member too. A reader points into its registry's events: it must not
  outlive its scene (a UI object kept across scene loads makes a new one).
- **Read every reader each update, before any early return.** A system that
  returns early on some frames (nothing to do, paused, waiting) and reads its
  events only afterwards will miss events once it skips two frames.
- Don't send a type while going through that type's range: the buffer may
  move. Collect what to send, then send it after the loop. Debug builds log
  an error (once per type) and end the loop; release builds don't check.

## When an event can be read

| Case | Rule |
| --- | --- |
| Sent in frame N (an update, lateUpdate, render, object, hook, scene load) | Readable until the frame boundary that starts frame N+2. |
| Sent by a worker | Taken in at the next frame boundary, then readable that frame and the next. |
| Who sees it | Each reader once, in its first `read()` after the send. A system earlier in the update order than the sender sees it the next step or frame: the earlier one reads what the later one wrote last frame. |
| Stepped systems | One swap a frame, not a step: a Stepped reader before the writer gets a step's events at its next step; a Frame or Real reader gets all the frame's steps' events once, on the last. |
| Order | Within one type, send order. A worker's events keep their own order and arrive at the frame boundary, ahead of that frame's main-thread ones. There's no order across types: what must stay ordered is one type. |
| Dropped | At the second frame boundary, read or not: a disabled system's, a reader made anew each update. An edit-mode scene isn't updated, so nothing is dropped there: what's sent in edit mode stays until play (or the 65,536-event warning). |
| A new reader | Starts at the oldest event kept, so nothing sent while a scene sets up is lost to the order things are made in. |

The frame boundary is `Events::update()`. `Scene::update()` calls it each frame
right after the flush, before the systems update; a headless registry's host
calls `registry.events().update()` once a frame, after `flushEntities()`.
Every enabled system updates at least once a frame (paused or at time scale 0
too, with a 0 s step), so an enabled system that reads each update misses
nothing.

**A miss is always logged.** When a reader's next `read()` finds events it
hadn't read already dropped, it starts at the oldest kept, logs a warning
naming the type and how many it missed (once per reader, in every build), and
the count adds to the type's *missed* total in the debug view. Where an event
carries a request or an answer some entity waits for, a miss would leave it
waiting: the warning is how you find it.

### Entities in events

An event names entities by `sfs::Entity`, which may be gone when it is read:
check it like any relationship handle (`if (changed.agent)`, a null
`getComponent`). Destroys take effect at the flush, before the frame boundary,
so an entity destroyed in frame N reads invalid from frame N+1, and a reused id
reads invalid by its generation.

## Threads

The owning thread sends, makes readers and senders, reads and updates. A
worker sends through an `EventSender<T>`:

```cpp
// On the owning thread, as the job is submitted:
sfs::EventSender<BakeDone> done = registry->events().sender<BakeDone>();
pool.submit([done, chunk] { /* ... */ done.send(BakeDone{chunk}); });
```

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
| Anything one system tells another: news, a notice, a request, an answer | An event. |
| A worker's news or result | An event through an `EventSender`. |

Never put a request field, a one-frame flag (`justChanged`, `done`) or a
message field on a component for another system to pick up. A system that
asks for something sends a request event; the serving system answers with an
event, and the asker writes what it needs into its own state. For now requests
and answers are plain events like any other; delivery policies for them (a
request taken by one consumer, only the latest per key kept) are coming.

Events don't replace `observe()` or `registry->componentChanges<T>()`: never
send an event per component write.

## Debugging

These go through the engine's [debug features](../../runtime/rendering/debug-drawing/index.md)
(the editor, Debug builds and development exports):

- `SFS_LOG_EVENTS=FieldCellChanged,AgentEvent` (names with or without their
  namespaces, or `*` for every type) logs each send's type, number and frame,
  each worker's send with its thread, and the workers' events as they're taken
  in. `debugFeatures::setLogEvents()` sets the list from code; it applies to
  types first used after it.
- A queue past 65,536 kept events warns once: a send loop, or a scene nobody
  updates.
- **Scene Settings > Debug > Events** shows, per type: sent last frame, kept,
  staged from workers, capacity and missed (`Events::stats()`).
- Profilers see the frame boundary as `events/update`.

In every build: a reader's missed events (above), and a call off the owning
thread.

## What it costs

| What | Layout | Allocates |
| --- | --- | --- |
| `Events` (in the registry) | queues by event type id, the queues in the order made, the owner thread's id, the frame count: 64 bytes | a queue at a type's first use |
| A queue | two `std::vector<T>` (each contiguous), the oldest kept event's number, which buffer is this frame's, the inbox pointer, debug counters (one layout in every build) | its buffers grow to the busiest frame, then keep their room |
| An inbox | a mutex, two vectors (staged, and the one handed back), an atomic count | once, at the first `sender()` |
| `EventReader<T>` / `EventSender<T>` | the queue, the next event's number, a byte: 24 bytes / a `std::shared_ptr`: 16 bytes | never |

Event type ids come from one table in engine-core keyed by the type's name, as
component ids do (`detail::eventTypeId()`), so the engine, the editor and a
game module agree.

A send is the thread check, the queue's lookup (a cached id, an index) and a
`push_back`; a read a few loads, then contiguous iteration. A frame boundary
skips an idle queue after a load or two; a busy one drops the older buffer
(nothing to do for trivially destructible events), flips which buffer is this
frame's and takes in what workers staged. Once both buffers have grown to the
busiest frame, nothing allocates.

`eventsBenchTests` (Release) measures it. It always prints its results and
gates, and checks the gates only when `SFS_BENCH_GATES` is set, in an
optimised build, so a loaded machine can't fail a test run. Where a plain
vector's own loop costs more than the read target on a machine, that gate is
set against the loop instead:

| Case | Target |
| --- | --- |
| send, a 32-byte trivially copyable event | ≤ 4 ns for an event built beforehand; a `send(T{...})` of a fresh temporary also pays for copying it (about 6 ns), as a plain `push_back` of one does, and stays ≤ 1.5x a `push_back` onto a component's vector |
| read, 1 and 4 readers | ≤ 0.5 ns per event per reader (or a plain vector's loop + 10 %) |
| `update()`, 32 idle queues / 10,000 trivially destructible events kept | ≤ 100 ns / the same as 10 kept (+5 %, or + 2 ns) |
| `EventSender::send`, 1 thread / 4 contending | ≤ 50 / ≤ 250 ns per event; taking them in ≤ 3 ns per event |
| 1,000 frames of steady traffic after the warm-up | no allocation (a static build counts them) |
