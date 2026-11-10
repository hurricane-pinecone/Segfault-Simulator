# Planning scheduler

Every path job of a scene runs on ONE worker pool: `sfs::nav::PlanScheduler`
(`engine/runtime/navigation/planScheduler.h`). `NavigationSystem` owns it
(`nav->scheduler()`); its fleet's plans (trips, wait spots, look-ahead), the
coarse graph build and a game's own planning (field plans, turns, line-ups)
all go through it. There are no other planning pools. A game that keeps job
handles beyond a frame (a route's job, a field plan's) holds the scheduler by
`nav->sharedScheduler()` (a `std::shared_ptr`; keep it as a `weak_ptr`), so a
handle dropped after the scene's gone cancels nothing that isn't there.

The problem it solves: with separate first-in-first-out pools, one vehicle's
long job (a 20 s field plan) held a thread while another vehicle's 50 ms turn
waited behind it. The scheduler runs everything at once, splits work into
small tasks, and serves whatever is needed soonest, with a fair turn per
vehicle.

## Pool

`hardware threads - 2` workers, at least 2 (`PlanScheduler::defaultThreads()`;
`Settings::threads` overrides it, for tests). No owner (an agent) holds more
than `threads - reserve` workers at once, so another's short job always finds
one free, however long this one's steps take. The workers run below normal
priority (`lowerThisThreadPriority()`, `engine/core/util/threadPriority.h`):
they may use every core they're given, but never take a time slice the game's
or the renderer's thread is waiting for (a frame preempted while it hands a
plan over was the game thread's worst planning cost).

## Jobs, steps and legs

A job is a step function the scheduler calls again and again until it returns
`Step::Done`: one leg, one pass, one candidate per call. Between two steps a
worker may take another job's task, so a long job never blocks a short one.

```cpp
nav::PlanScheduler& s = nav->scheduler();
nav::PlanScheduler::Request r;
r.owner = agentId;                 // the fair turn is per owner
r.label = "Combine 1: field 3";    // for the log
r.deadline = whenItRunsOutOfPath;  // needed by (Clock::now(): standing)
r.step = [state](nav::PlanScheduler::Context& ctx) {
  ctx.publish(planNextPass(*state)); // a leg: streamed, never changed
  return state->more() ? nav::PlanScheduler::Step::More
                       : nav::PlanScheduler::Step::Done;
};
const auto job = s.submit(std::move(r));

// Main thread, each frame: the legs published since last time, in order.
nav::PlanScheduler::Collected c = s.collect(job);
for (const std::any& leg : c.legs)
  drive(std::any_cast<const Pass&>(leg));
```

- **Streaming.** `publish()` hands a leg over as soon as it's ready; legs come
  out of `collect()` in the order published, each once, and a published leg
  never changes. A finished job (`Done` or `Failed`) is forgotten once it's
  collected with nothing left.
- **One-step jobs.** `submitOne(owner, label, deadline, work)` publishes the
  one result of `work`. `nav::vehicle::submitPlan(scheduler, owner, label,
  deadline, PlanRequest)` is one vehicle plan: its leg is the `PlanResult`
  `planNow()` gives (same function, same answer), never the clear way so far.
- **Parallel tasks inside a job.** A step may `fork()` tasks that run on any
  free workers (a field plan's layout candidates); the job's next step runs
  once they have all finished (and picks the best, exactly as it would one
  after another).
- **Cancel.** `cancel(job)`: its queued tasks never run, a running step sees
  `ctx.cancelled()` (pass `ctx.cancelFlag()` as a `PlanRequest::cancel`) and
  nothing more is published.
- **Errors.** A step that throws is caught and logged (`LOG_ERROR`), its job
  ends `Failed`; nothing escapes into the game loop.
- **Letting go off the game thread.** `release(object)` hands something heavy
  (a finished plan's copy of the world, its checkers) to a worker, which drops
  the last hold on it: freeing it never costs the game thread. A job forgotten
  by `collect()` (finished) or `cancel()` is let go the same way. Make heavy
  per-job state on the worker too (in the job's first step), so the game
  thread's part of planning is only handing requests over and taking legs in.

## Which task goes next

When a worker is free it takes, in order:

1. **Escalated** tasks first: a queued task that will miss its deadline (now +
   its expected step time is past it) has its deadline made now. The expected
   step time starts at `Request::estimate` and is measured as the job runs.
2. Then the **owner with fewer tasks running** (a fair turn per vehicle).
3. Then the **earliest deadline**, then the order asked.

The deadline is when the result is needed by: now for a standing vehicle,
otherwise when it runs out of known path. `setDeadline(job, t)` changes it (a
vehicle stopped early).

**Overload.** Look-ahead jobs (`Request::lookAhead`: the next leg of a vehicle
with plenty of path left) are shed first: one with more than
`Settings::shedSlack` seconds to spare isn't started while that would leave
fewer than `Settings::reserve` workers free. It starts when the load drops, or
when its deadline comes close (and is escalated if it's then going to miss).

## Missed deadlines

- Nothing unplanned is ever driven. A vehicle whose next leg is late eases off
  and stands at the end of what it has: the speed profile's normal stop. It
  pulls away when the leg comes in.
- No partial legs: a search's "clear way so far" is never published or driven.
- Every leg published after its job's deadline is a miss: counted in `stats()`
  (`misses`, `latestMiss`) and logged once it's over `Settings::logLateAfter`
  (0.25 s) with the real seconds it was late, which is how long the vehicle
  stood waiting for it:
  `Planning: 'agent 3: trip' leg 1 came 0.41 s after it was needed (0.41 s after it was asked)`.
- No way at all is not a miss: the planner proves `Blocked` and the agent's
  usual handling (wait, plan again, report blocked) applies.
- Standing while a route is still being planned is not a failure.

## Look-ahead

`nav::Fleet::planAhead(id, ask)` (`NavigationSystem::planAhead(entity, ask)`)
plans an agent's next way from where its current path ends (its last sample's
pose and hitch), needed by when it gets there: its speed profile's time left,
in real seconds at the game's measured pace. `ahead(id)` hands the finished
plan over once (none meanwhile); the game drives it with `setPath()` when the
current path ends. One look-ahead per agent at a time: a new one replaces it.

## Tests

`engine/tests/runtime/navigation/schedulerTests.cpp`: earliest deadline first,
escalation of a job that will miss, fairness (a big job never starves a short
one), streaming order, cancel, look-ahead shedding under load, parallel tasks
inside a job, errors, misses, and a benchmark (one long job of several seconds
beside sixty short ones: the short ones' p50/p95 latency alone and beside it).
`fleetTests` checks no partial leg is ever driven and the look-ahead hand-over.
