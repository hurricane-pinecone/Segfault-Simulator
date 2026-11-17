#include "../../testHarness.h"
#include "engine/runtime/navigation/planScheduler.h"

#include <any>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// Every check here holds however fast the machine runs: the schedulers go by
// a test clock that only moves when a test moves it, and a test waits on what
// it checks, never for a while. Something that should happen and never does
// shows as a hang, which ctest's timeout reports.
using namespace sfs::nav;
using Clock = PlanScheduler::Clock;
using namespace std::chrono_literals;

namespace
{
Clock::duration toDuration(double seconds)
{
  return std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(seconds));
}

// The time a scheduler under test goes by: still unless a test moves it.
struct TestClock
{
  std::atomic<Clock::rep> ticks{Clock::now().time_since_epoch().count()};

  void advance(double seconds) { ticks += toDuration(seconds).count(); }

  Clock::time_point in(double seconds) const
  {
    return now() + toDuration(seconds);
  }

  Clock::time_point now() const
  {
    return Clock::time_point(Clock::duration(ticks.load()));
  }
};

// Waits till `done()` says so.
template <typename Done>
void waitUntil(Done done)
{
  while (!done())
    std::this_thread::sleep_for(1ms);
}

// A worker held till it's let go (the queue builds up behind it).
struct Gate
{
  std::atomic<bool> open{false};
  std::atomic<bool> holding{false};

  void hold(PlanScheduler& s, Clock::time_point deadline, uint64_t owner = 999)
  {
    s.submitOne(owner,
                "gate",
                deadline,
                [this](const std::atomic<bool>&)
                {
                  holding = true;
                  waitUntil([this] { return open.load(); });
                  return std::any(0);
                });
    waitUntil([this] { return holding.load(); });
  }
};

PlanScheduler::Settings on(TestClock& clock, unsigned threads)
{
  PlanScheduler::Settings settings;
  settings.threads = threads;
  settings.now = [&clock] { return clock.now(); };
  return settings;
}

// Waits till the job's done or failed.
void waitFor(PlanScheduler& s, PlanScheduler::JobId id)
{
  waitUntil(
      [&]
      {
        const PlanScheduler::State st = s.state(id);
        return st == PlanScheduler::State::Done ||
               st == PlanScheduler::State::Failed;
      });
}
} // namespace

int main()
{
  TEST("its pool should be the machine's hardware threads - 2, at least 2")
  {
    PlanScheduler s;
    CHECK(s.threads() == PlanScheduler::defaultThreads());
    CHECK(s.threads() >= 2u);
  }

  TEST("earliest deadline first")
  {
    TestClock clock;
    PlanScheduler s(on(clock, 1));
    Gate gate;
    gate.hold(s, clock.now());
    std::mutex m;
    std::vector<int> order;
    std::vector<PlanScheduler::JobId> ids;
    const double deadlines[] = {3.0, 1.0, 4.0, 2.0};

    for (int i = 0; i < 4; ++i)
      ids.push_back(s.submitOne(static_cast<uint64_t>(i + 1),
                                "job",
                                clock.in(deadlines[i]),
                                [&, i](const std::atomic<bool>&)
                                {
                                  const std::lock_guard lock(m);
                                  order.push_back(i);
                                  return std::any(i);
                                }));

    gate.open = true;

    for (const auto id : ids)
      waitFor(s, id);

    CHECK(order == std::vector<int>({1, 3, 0, 2}));
    CHECK(s.stats().escalated == 1u); // (the gate only: asked for now)
  }

  TEST("a job that will miss its deadline is escalated to the top")
  {
    TestClock clock;
    PlanScheduler s(on(clock, 1));
    Gate gate;
    gate.hold(s, clock.now());
    std::mutex m;
    std::vector<int> order;
    const auto record = [&](int i)
    {
      return [&, i](PlanScheduler::Context& ctx)
      {
        const std::lock_guard lock(m);
        order.push_back(i);
        ctx.publish(i);
        return PlanScheduler::Step::Done;
      };
    };
    // 0: needed in 0.5 s, a quick one. 1: needed later, in 1 s, but its step
    // takes 2 s: it can't make it, its deadline's now.
    PlanScheduler::Request quick;
    quick.owner = 1;
    quick.deadline = clock.in(0.5);
    quick.step = record(0);
    PlanScheduler::Request slow;
    slow.owner = 2;
    slow.deadline = clock.in(1.0);
    slow.estimate = 2.0f;
    slow.step = record(1);
    const auto a = s.submit(quick);
    const auto b = s.submit(slow);
    gate.open = true;
    waitFor(s, a);
    waitFor(s, b);
    CHECK(order == std::vector<int>({1, 0}));
    CHECK(s.stats().escalated == 2u); // (the gate and the slow one)
  }

  TEST("fairness: one owner's work never holds every worker, so another's "
       "short job runs beside it")
  {
    // Two workers, one kept back: an owner holds at most one.
    TestClock clock;
    PlanScheduler s(on(clock, 2));
    std::atomic<bool> released{false};
    std::atomic<int> started{0}, running{0}, mostRunning{0};
    // Owner 1's work, each part held till it's let go.
    const auto held = [&]
    {
      ++started;
      const int now = ++running;
      int most = mostRunning;

      while (now > most && !mostRunning.compare_exchange_weak(most, now))
      {
      }

      waitUntil([&] { return released.load(); });
      --running;
    };
    // Owner 1: a coverage plan's shape (as many parallel tasks as there are
    // workers) and six plain plans, all needed now.
    std::vector<PlanScheduler::JobId> big;
    PlanScheduler::Request coverage;
    coverage.owner = 1;
    coverage.label = "coverage";
    coverage.deadline = clock.now();
    coverage.step =
        [&, forked = std::make_shared<bool>(false)](PlanScheduler::Context& ctx)
    {
      if (*forked)
        return PlanScheduler::Step::Done;

      *forked = true;

      for (unsigned i = 0; i < s.threads(); ++i)
        ctx.fork([&](const std::atomic<bool>&) { held(); });

      return PlanScheduler::Step::More;
    };
    big.push_back(s.submit(coverage));

    for (int i = 0; i < 6; ++i)
      big.push_back(s.submitOne(1,
                                "big",
                                clock.now(),
                                [&](const std::atomic<bool>&)
                                {
                                  held();
                                  return std::any(0);
                                }));

    waitUntil([&] { return started.load() > 0; });
    // Owner 2: one short one, asked after them all. It's done while owner 1's
    // one running part is still held.
    const auto small =
        s.submitOne(2,
                    "short",
                    clock.now(),
                    [](const std::atomic<bool>&) { return std::any(0); });
    waitFor(s, small);
    CHECK(started == 1);
    released = true;

    for (const auto id : big)
      waitFor(s, id);

    CHECK(mostRunning == 1);
  }

  TEST("streamed legs come in order, each once, never changed, the first "
       "before the job's done")
  {
    PlanScheduler s;
    PlanScheduler::Request r;
    r.owner = 1;
    r.label = "stream";
    // Each step waits for its leg to be collected, so the job's still running
    // when the first one is.
    std::atomic<std::size_t> seen{0};
    auto next = std::make_shared<std::size_t>(0);
    r.step = [next, &seen](PlanScheduler::Context& ctx)
    {
      ctx.publish(std::vector<int>(3, static_cast<int>(*next))); // (a leg)
      const std::size_t published = ++*next;
      waitUntil([&] { return seen.load() >= published; });
      return published < 5 ? PlanScheduler::Step::More
                           : PlanScheduler::Step::Done;
    };
    const auto id = s.submit(r);
    std::vector<std::vector<int>> legs;
    bool firstBeforeDone = false;

    for (;;)
    {
      const PlanScheduler::Collected c = s.collect(id);

      for (const std::any& leg : c.legs)
        legs.push_back(std::any_cast<std::vector<int>>(leg));

      seen = legs.size();

      if (!legs.empty() && c.state == PlanScheduler::State::Running)
        firstBeforeDone = true;

      if (c.state == PlanScheduler::State::Done ||
          c.state == PlanScheduler::State::Unknown)
        break;

      std::this_thread::sleep_for(1ms);
    }

    CHECK(firstBeforeDone);
    CHECK(legs.size() == 5u);

    for (std::size_t i = 0; i < legs.size(); ++i)
      CHECK(legs[i] == std::vector<int>(3, static_cast<int>(i)));

    // (Collected and done: forgotten.)
    CHECK(s.state(id) == PlanScheduler::State::Unknown);
  }

  TEST("cancel: a queued job never runs, a running one stops and publishes "
       "nothing more")
  {
    TestClock clock;
    PlanScheduler s(on(clock, 2));
    std::atomic<int> ranQueued{0};
    std::atomic<int> steps{0};
    std::atomic<bool> sawCancel{false};
    PlanScheduler::Request r;
    r.owner = 1;
    r.deadline = clock.now();
    r.step = [&](PlanScheduler::Context& ctx)
    {
      ++steps;
      waitUntil([&] { return ctx.cancelled(); });
      sawCancel = true;
      ctx.publish(1);
      return PlanScheduler::Step::More;
    };
    const auto running = s.submit(r);
    waitUntil([&] { return steps.load() > 0; });
    Gate gate;
    gate.hold(s, clock.now(), 7); // (the other worker held)
    // Both workers are busy, so this one's still queued when it's cancelled.
    const auto queued = s.submitOne(2,
                                    "queued",
                                    clock.now(),
                                    [&](const std::atomic<bool>&)
                                    {
                                      ++ranQueued;
                                      return std::any(0);
                                    });
    s.cancel(queued);
    s.cancel(running);
    gate.open = true;
    waitUntil([&] { return s.pending() == 0u; });
    CHECK(ranQueued == 0);
    CHECK(sawCancel);
    CHECK(steps == 1);
    CHECK(s.state(running) == PlanScheduler::State::Unknown);
    CHECK(s.collect(running).legs.empty());
  }

  TEST("wanted: a job that's no longer wanted runs no further step")
  {
    TestClock clock;
    PlanScheduler s(on(clock, 2));
    std::atomic<bool> wanted{true};
    std::atomic<int> steps{0};
    PlanScheduler::Request r;
    r.owner = 1;
    r.deadline = clock.now();
    r.wanted = [&] { return wanted.load(); };
    // The third step ends once the job's no longer wanted.
    r.step = [&](PlanScheduler::Context& ctx)
    {
      ctx.publish(++steps);

      if (steps == 3)
        waitUntil([&] { return !wanted.load(); });

      return steps < 1000 ? PlanScheduler::Step::More
                          : PlanScheduler::Step::Done;
    };
    const auto job = s.submit(r);
    waitUntil([&] { return steps.load() == 3; });
    wanted = false;
    waitUntil([&] { return s.state(job) == PlanScheduler::State::Unknown; });
    waitUntil([&] { return s.pending() == 0u; });
    CHECK(steps == 3);
    // One never wanted never runs: it's dropped before the job asked after it
    // is taken.
    std::atomic<int> ran{0};
    PlanScheduler::Request never;
    never.owner = 2;
    never.deadline = clock.now();
    never.wanted = [] { return false; };
    never.step = [&](PlanScheduler::Context&)
    {
      ++ran;
      return PlanScheduler::Step::Done;
    };
    const auto dropped = s.submit(never);
    const auto after =
        s.submitOne(3,
                    "after",
                    clock.now(),
                    [](const std::atomic<bool>&) { return std::any(0); });
    waitFor(s, after);
    CHECK(ran == 0);
    CHECK(s.state(dropped) == PlanScheduler::State::Unknown);
  }

  TEST("under load, look-ahead work with plenty of slack is deferred first; "
       "it runs once the load drops")
  {
    TestClock clock;
    PlanScheduler::Settings settings = on(clock, 2);
    settings.shedSlack = 5.0f;
    PlanScheduler s(settings);
    Gate gate;
    // (One worker busy: the last one's kept for what's needed.)
    gate.hold(s, clock.now());
    std::atomic<bool> aheadRan{false}, urgentRan{false}, closeRan{false};
    const auto ahead = s.submitOne(
        1,
        "ahead",
        clock.in(30.0),
        [&](const std::atomic<bool>&)
        {
          aheadRan = true;
          return std::any(0);
        },
        true);
    const auto close = s.submitOne(
        2,
        "ahead, close",
        clock.in(1.0), // (within shedSlack: not deferred)
        [&](const std::atomic<bool>&)
        {
          closeRan = true;
          return std::any(0);
        },
        true);
    const auto urgent = s.submitOne(3,
                                    "urgent",
                                    clock.now(),
                                    [&](const std::atomic<bool>&)
                                    {
                                      urgentRan = true;
                                      return std::any(0);
                                    });
    waitFor(s, urgent);
    waitFor(s, close);
    CHECK(urgentRan);
    CHECK(closeRan);
    CHECK(!aheadRan);
    CHECK(s.stats().deferred == 1u);
    gate.open = true; // (the load drops)
    waitFor(s, ahead);
    CHECK(aheadRan);
  }

  TEST("parallel tasks inside a job: they run side by side, and its next "
       "step waits for all of them")
  {
    // A worker for each task whatever the machine. Each task waits till all
    // three have started, so they only finish if they ran side by side.
    PlanScheduler::Settings settings;
    settings.threads = 4;
    PlanScheduler s(settings);
    auto results = std::make_shared<std::vector<int>>(3, 0);
    auto arrived = std::make_shared<std::atomic<int>>(0);
    auto phase = std::make_shared<int>(0);
    PlanScheduler::Request r;
    r.owner = 1;
    r.step = [results, arrived, phase](PlanScheduler::Context& ctx)
    {
      if ((*phase)++ == 0)
      {
        for (int i = 0; i < 3; ++i)
          ctx.fork(
              [results, arrived, i](const std::atomic<bool>&)
              {
                ++*arrived;
                waitUntil([&] { return arrived->load() == 3; });
                (*results)[static_cast<std::size_t>(i)] = i + 1;
              });

        return PlanScheduler::Step::More;
      }

      ctx.publish((*results)[0] + (*results)[1] + (*results)[2]);
      return PlanScheduler::Step::Done;
    };
    const auto id = s.submit(r);
    int sum = 0;

    for (;;)
    {
      const PlanScheduler::Collected c = s.collect(id);

      for (const std::any& leg : c.legs)
        sum = std::any_cast<int>(leg);

      if (c.state == PlanScheduler::State::Done)
        break;

      std::this_thread::sleep_for(1ms);
    }

    CHECK(sum == 6);
  }

  TEST("a step that throws fails its job, logged; nothing escapes")
  {
    PlanScheduler s;
    const auto id = s.submitOne(1,
                                "throws",
                                Clock::now(),
                                [](const std::atomic<bool>&) -> std::any
                                { throw std::runtime_error("test error"); });
    waitFor(s, id);
    CHECK(s.collect(id).state == PlanScheduler::State::Failed);
    // (And it goes on.)
    const auto ok =
        s.submitOne(1,
                    "fine",
                    Clock::now(),
                    [](const std::atomic<bool>&) { return std::any(1); });
    waitFor(s, ok);
    CHECK(s.collect(ok).state == PlanScheduler::State::Done);
  }

  TEST("a leg published after its deadline is a miss, counted")
  {
    TestClock clock;
    PlanScheduler s(on(clock, 2));
    // On time: no miss.
    const auto onTime =
        s.submitOne(1,
                    "on time",
                    clock.now(),
                    [](const std::atomic<bool>&) { return std::any(0); });
    waitFor(s, onTime);
    CHECK(s.stats().misses == 0u);
    // Its search takes 0.3 s.
    const auto late = s.submitOne(1,
                                  "late",
                                  clock.now(),
                                  [&clock](const std::atomic<bool>&)
                                  {
                                    clock.advance(0.3);
                                    return std::any(0);
                                  });
    waitFor(s, late);
    CHECK(s.stats().misses == 1u);
    CHECK(std::abs(s.stats().latestMiss - 0.3) < 1e-6);
  }

  TEST("release: what's let go is freed on a worker, never the caller's "
       "thread; a job forgotten (collected finished, cancelled) the same")
  {
    struct Heavy
    {
      explicit Heavy(std::atomic<std::thread::id>* f) : freedOn(f) {}
      Heavy(const Heavy&) = delete;
      std::atomic<std::thread::id>* freedOn;
      ~Heavy() { freedOn->store(std::this_thread::get_id()); }
    };
    PlanScheduler s;
    std::atomic<std::thread::id> freed{};
    s.release(std::make_shared<Heavy>(&freed));
    waitUntil([&] { return freed.load() != std::thread::id{}; });
    CHECK(freed.load() != std::this_thread::get_id());
    // (A finished job's step held it last: collected, freed on a worker.)
    std::atomic<std::thread::id> jobFreed{};
    {
      auto held = std::make_shared<Heavy>(&jobFreed);
      const PlanScheduler::JobId id =
          s.submitOne(1,
                      "holds",
                      Clock::now(),
                      [held](const std::atomic<bool>&) { return std::any(0); });
      held.reset();
      waitFor(s, id);
      s.collect(id);
    }
    waitUntil([&] { return jobFreed.load() != std::thread::id{}; });
    CHECK(jobFreed.load() != std::this_thread::get_id());
  }

  TEST("collected false: a job is forgotten as it ends (done, failed, "
       "cancelled), what it holds freed on a worker")
  {
    struct Heavy
    {
      explicit Heavy(std::atomic<int>* f, std::atomic<bool>* other)
          : freed(f), onOtherThread(other), made(std::this_thread::get_id())
      {
      }
      Heavy(const Heavy&) = delete;
      std::atomic<int>* freed;
      std::atomic<bool>* onOtherThread;
      std::thread::id made;
      ~Heavy()
      {
        if (std::this_thread::get_id() != made)
          *onOtherThread = true;

        ++*freed;
      }
    };
    PlanScheduler::Settings settings;
    settings.threads = 2;
    PlanScheduler s(settings);
    std::atomic<int> freed{0};
    std::atomic<bool> onWorker{false};
    std::atomic<int> ended{0};
    const auto uncollected = [&](uint64_t owner, int outcome)
    {
      PlanScheduler::Request r;
      r.owner = owner;
      r.label = "answers by responder";
      r.collected = false;
      r.step = [held = std::make_shared<Heavy>(&freed, &onWorker),
                outcome,
                &ended](PlanScheduler::Context& ctx)
      {
        if (outcome == 2)
        {
          waitUntil([&] { return ctx.cancelled(); });
          return PlanScheduler::Step::Done;
        }

        ++ended;

        if (outcome == 1)
          throw std::runtime_error("no way through");

        return PlanScheduler::Step::Done;
      };
      return s.submit(std::move(r));
    };
    const PlanScheduler::JobId done = uncollected(1, 0);
    const PlanScheduler::JobId failed = uncollected(2, 1);
    const PlanScheduler::JobId cancelled = uncollected(3, 2);
    waitUntil([&] { return ended.load() == 2; });
    s.cancel(cancelled);
    waitUntil([&] { return freed.load() == 3; });
    CHECK(onWorker.load());
    CHECK(s.state(done) == PlanScheduler::State::Unknown);
    CHECK(s.state(failed) == PlanScheduler::State::Unknown);
    CHECK(s.state(cancelled) == PlanScheduler::State::Unknown);
    waitUntil([&] { return s.pending() == 0u; });
    // A collected job (the default) still waits for its collect().
    const PlanScheduler::JobId kept =
        s.submitOne(4,
                    "kept",
                    Clock::now(),
                    [](const std::atomic<bool>&) { return std::any(1); });
    waitFor(s, kept);
    CHECK(s.state(kept) == PlanScheduler::State::Done);
    CHECK(s.collect(kept).legs.size() == 1u);
    CHECK(s.state(kept) == PlanScheduler::State::Unknown);
  }

  return testing::report("schedulerTests");
}
