#include "../../testHarness.h"

#include <engine/runtime/navigation/planScheduler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace sfs::nav;
using Clock = PlanScheduler::Clock;
using namespace std::chrono_literals;

namespace
{
// A worker held till it's let go (the queue builds up behind it).
struct Gate
{
  std::atomic<bool> open{false};
  std::atomic<bool> holding{false};
  void hold(PlanScheduler& s, uint64_t owner = 999)
  {
    s.submitOne(owner,
                "gate",
                Clock::now(),
                [this](const std::atomic<bool>&)
                {
                  holding = true;
                  while (!open)
                    std::this_thread::sleep_for(1ms);
                  return std::any(0);
                });
    while (!holding)
      std::this_thread::sleep_for(1ms);
  }
};

// Busy for `seconds` (a search's worth of work).
void work(double seconds)
{
  const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double>(seconds));
  while (Clock::now() < end)
  {
  }
}

bool waitFor(PlanScheduler& s, PlanScheduler::JobId id, double seconds = 10.0)
{
  const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double>(seconds));
  while (Clock::now() < end)
  {
    const PlanScheduler::State st = s.state(id);
    if (st == PlanScheduler::State::Done || st == PlanScheduler::State::Failed)
      return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

Clock::time_point in(double seconds)
{
  return Clock::now() + std::chrono::duration_cast<Clock::duration>(
                            std::chrono::duration<double>(seconds));
}

double ms(Clock::time_point a, Clock::time_point b)
{
  return std::chrono::duration<double, std::milli>(b - a).count();
}

double percentile(std::vector<double> v, double p)
{
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t i =
      std::min(v.size() - 1,
               static_cast<std::size_t>(p * static_cast<double>(v.size())));
  return v[i];
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
    PlanScheduler::Settings settings;
    settings.threads = 1;
    PlanScheduler s(settings);
    Gate gate;
    gate.hold(s);
    std::mutex m;
    std::vector<int> order;
    std::vector<PlanScheduler::JobId> ids;
    const double deadlines[] = {3.0, 1.0, 4.0, 2.0};
    for (int i = 0; i < 4; ++i)
      ids.push_back(s.submitOne(static_cast<uint64_t>(i + 1),
                                "job",
                                in(deadlines[i]),
                                [&, i](const std::atomic<bool>&)
                                {
                                  const std::lock_guard lock(m);
                                  order.push_back(i);
                                  return std::any(i);
                                }));
    gate.open = true;
    for (const auto id : ids)
      CHECK(waitFor(s, id));
    CHECK(order == std::vector<int>({1, 3, 0, 2}));
    CHECK(s.stats().escalated == 1u); // (the gate only: asked for now)
  }

  TEST("a job that will miss its deadline is escalated to the top")
  {
    PlanScheduler::Settings settings;
    settings.threads = 1;
    PlanScheduler s(settings);
    Gate gate;
    gate.hold(s);
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
    quick.deadline = in(0.5);
    quick.step = record(0);
    PlanScheduler::Request slow;
    slow.owner = 2;
    slow.deadline = in(1.0);
    slow.estimate = 2.0f;
    slow.step = record(1);
    const auto a = s.submit(quick);
    const auto b = s.submit(slow);
    gate.open = true;
    CHECK(waitFor(s, a));
    CHECK(waitFor(s, b));
    CHECK(order == std::vector<int>({1, 0}));
    CHECK(s.stats().escalated >= 2u); // (the gate and the slow one)
  }

  TEST("fairness: one owner's big job never starves another's short one")
  {
    PlanScheduler::Settings settings;
    settings.threads = 2;
    PlanScheduler s(settings);
    // Owner 1: six 200 ms plans, all needed now.
    std::vector<PlanScheduler::JobId> big;
    for (int i = 0; i < 6; ++i)
      big.push_back(s.submitOne(1,
                                "big",
                                Clock::now(),
                                [](const std::atomic<bool>&)
                                {
                                  work(0.2);
                                  return std::any(0);
                                }));
    std::this_thread::sleep_for(20ms);
    // Owner 2: one short one, asked after them all.
    const auto asked = Clock::now();
    Clock::time_point ran{};
    const auto small = s.submitOne(2,
                                   "short",
                                   Clock::now(),
                                   [&](const std::atomic<bool>&)
                                   {
                                     ran = Clock::now();
                                     return std::any(0);
                                   });
    CHECK(waitFor(s, small));
    const double waited = ms(asked, ran);
    std::printf(
        "    (the short one started %.1f ms after it was asked)\n", waited);
    // (One owner holds at most threads - reserve workers: the other's free.)
    CHECK(waited < 50.0);
    for (const auto id : big)
      CHECK(waitFor(s, id));
  }

  TEST("streamed legs come in order, each once, never changed, the first "
       "before the job's done")
  {
    PlanScheduler s;
    PlanScheduler::Request r;
    r.owner = 1;
    r.label = "stream";
    r.deadline = Clock::now();
    auto next = std::make_shared<int>(0);
    r.step = [next](PlanScheduler::Context& ctx)
    {
      std::this_thread::sleep_for(30ms);
      ctx.publish(std::vector<int>(3, *next)); // (a leg: three of its index)
      return ++*next < 5 ? PlanScheduler::Step::More
                         : PlanScheduler::Step::Done;
    };
    const auto id = s.submit(r);
    std::vector<std::vector<int>> legs;
    bool firstBeforeDone = false;
    for (int i = 0; i < 5000; ++i)
    {
      const PlanScheduler::Collected c = s.collect(id);
      for (const std::any& leg : c.legs)
        legs.push_back(std::any_cast<std::vector<int>>(leg));
      if (!legs.empty() && c.state == PlanScheduler::State::Running)
        firstBeforeDone = true;
      if (c.state == PlanScheduler::State::Done ||
          c.state == PlanScheduler::State::Unknown)
        break;
      std::this_thread::sleep_for(2ms);
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
    PlanScheduler::Settings settings;
    settings.threads = 2;
    PlanScheduler s(settings);
    std::atomic<int> ranQueued{0};
    std::atomic<int> steps{0};
    std::atomic<bool> sawCancel{false};
    PlanScheduler::Request r;
    r.owner = 1;
    r.step = [&](PlanScheduler::Context& ctx)
    {
      ++steps;
      for (int i = 0; i < 200 && !ctx.cancelled(); ++i)
        std::this_thread::sleep_for(1ms);
      sawCancel = sawCancel || ctx.cancelled();
      ctx.publish(1);
      return PlanScheduler::Step::More;
    };
    const auto running = s.submit(r);
    while (steps == 0)
      std::this_thread::sleep_for(1ms);
    Gate gate;
    gate.hold(s, 7); // (the other worker held)
    const auto queued = s.submitOne(2,
                                    "queued",
                                    Clock::now(),
                                    [&](const std::atomic<bool>&)
                                    {
                                      ++ranQueued;
                                      return std::any(0);
                                    });
    s.cancel(queued);
    s.cancel(running);
    gate.open = true;
    std::this_thread::sleep_for(300ms);
    CHECK(ranQueued == 0);
    CHECK(sawCancel);
    CHECK(steps == 1);
    CHECK(s.state(running) == PlanScheduler::State::Unknown);
    CHECK(s.collect(running).legs.empty());
    CHECK(s.pending() == 0u);
  }

  TEST("under load, look-ahead work with plenty of slack is deferred first; "
       "it runs once the load drops, or when its deadline comes close")
  {
    PlanScheduler::Settings settings;
    settings.threads = 2;
    settings.shedSlack = 5.0f;
    PlanScheduler s(settings);
    Gate gate;
    gate.hold(s); // (one worker busy: the last one's kept for what's needed)
    std::atomic<bool> aheadRan{false}, urgentRan{false}, closeRan{false};
    const auto ahead = s.submitOne(
        1,
        "ahead",
        in(30.0),
        [&](const std::atomic<bool>&)
        {
          aheadRan = true;
          return std::any(0);
        },
        true);
    const auto close = s.submitOne(
        2,
        "ahead, close",
        in(1.0), // (within shedSlack: not deferred)
        [&](const std::atomic<bool>&)
        {
          closeRan = true;
          return std::any(0);
        },
        true);
    const auto urgent = s.submitOne(3,
                                    "urgent",
                                    Clock::now(),
                                    [&](const std::atomic<bool>&)
                                    {
                                      urgentRan = true;
                                      return std::any(0);
                                    });
    CHECK(waitFor(s, urgent, 2.0));
    CHECK(waitFor(s, close, 2.0));
    std::this_thread::sleep_for(100ms);
    CHECK(urgentRan);
    CHECK(closeRan);
    CHECK(!aheadRan);
    CHECK(s.stats().deferred == 1u);
    gate.open = true; // (the load drops)
    CHECK(waitFor(s, ahead, 2.0));
    CHECK(aheadRan);
  }

  TEST("parallel tasks inside a job: its next step waits for all of them")
  {
    PlanScheduler s;
    auto results = std::make_shared<std::vector<int>>(3, 0);
    auto phase = std::make_shared<int>(0);
    PlanScheduler::Request r;
    r.owner = 1;
    r.deadline = Clock::now();
    r.step = [results, phase](PlanScheduler::Context& ctx)
    {
      if ((*phase)++ == 0)
      {
        for (int i = 0; i < 3; ++i)
          ctx.fork(
              [results, i](const std::atomic<bool>&)
              {
                std::this_thread::sleep_for(150ms);
                (*results)[static_cast<std::size_t>(i)] = i + 1;
              });
        return PlanScheduler::Step::More;
      }
      ctx.publish((*results)[0] + (*results)[1] + (*results)[2]);
      return PlanScheduler::Step::Done;
    };
    const auto t0 = Clock::now();
    const auto id = s.submit(r);
    int sum = 0;
    for (int i = 0; i < 5000; ++i)
    {
      const PlanScheduler::Collected c = s.collect(id);
      for (const std::any& leg : c.legs)
        sum = std::any_cast<int>(leg);
      if (c.state == PlanScheduler::State::Done)
        break;
      std::this_thread::sleep_for(1ms);
    }
    const double took = ms(t0, Clock::now());
    std::printf("    (3 x 150 ms in parallel: %.0f ms)\n", took);
    CHECK(sum == 6);
    CHECK(took < 400.0);
  }

  TEST("a step that throws fails its job, logged; nothing escapes")
  {
    PlanScheduler s;
    const auto id = s.submitOne(1,
                                "throws",
                                Clock::now(),
                                [](const std::atomic<bool>&) -> std::any
                                { throw std::runtime_error("test error"); });
    CHECK(waitFor(s, id));
    CHECK(s.collect(id).state == PlanScheduler::State::Failed);
    // (And it goes on.)
    const auto ok =
        s.submitOne(1,
                    "fine",
                    Clock::now(),
                    [](const std::atomic<bool>&) { return std::any(1); });
    CHECK(waitFor(s, ok));
  }

  TEST("a leg published after its deadline is a miss, counted")
  {
    PlanScheduler s;
    const auto id = s.submitOne(1,
                                "late",
                                Clock::now(),
                                [](const std::atomic<bool>&)
                                {
                                  work(0.3);
                                  return std::any(0);
                                });
    CHECK(waitFor(s, id));
    CHECK(s.stats().misses == 1u);
    CHECK(s.stats().latestMiss > 0.25);
  }

  TEST("benchmark: a long job (seconds) beside many short ones: the short "
       "ones' latency is unaffected")
  {
    // Short: 60 plans of 20 ms from 6 owners, one every 25 ms, needed now.
    const auto shorts = [](PlanScheduler& s)
    {
      std::vector<double> latency;
      std::mutex m;
      std::vector<PlanScheduler::JobId> ids;
      for (int i = 0; i < 60; ++i)
      {
        const auto asked = Clock::now();
        ids.push_back(s.submitOne(static_cast<uint64_t>(10 + i % 6),
                                  "short",
                                  Clock::now(),
                                  [&, asked](const std::atomic<bool>&)
                                  {
                                    work(0.02);
                                    const std::lock_guard lock(m);
                                    latency.push_back(ms(asked, Clock::now()));
                                    return std::any(0);
                                  }));
        std::this_thread::sleep_for(25ms);
      }
      for (const auto id : ids)
        waitFor(s, id);
      return latency;
    };
    PlanScheduler::Settings settings;
    settings.logLateAfter = 1.0e9f; // (no log lines from the benchmark)
    double alone50 = 0, alone95 = 0, with50 = 0, with95 = 0;
    {
      PlanScheduler s(settings);
      const std::vector<double> l = shorts(s);
      alone50 = percentile(l, 0.5);
      alone95 = percentile(l, 0.95);
    }
    {
      PlanScheduler s(settings);
      // Long: a field plan's shape: as many layout candidates as there are
      // workers, 1.5 s each, in parallel, then 10 passes of 150 ms.
      auto phase = std::make_shared<int>(0);
      PlanScheduler::Request r;
      r.owner = 1;
      r.label = "field";
      r.deadline = Clock::now();
      const unsigned forks = s.threads();
      r.step = [phase, forks](PlanScheduler::Context& ctx)
      {
        if ((*phase)++ == 0)
        {
          for (unsigned i = 0; i < forks; ++i)
            ctx.fork([](const std::atomic<bool>&) { work(1.5); });
          return PlanScheduler::Step::More;
        }
        work(0.15);
        ctx.publish(*phase);
        return *phase <= 10 ? PlanScheduler::Step::More
                            : PlanScheduler::Step::Done;
      };
      const auto t0 = Clock::now();
      const auto field = s.submit(r);
      std::this_thread::sleep_for(50ms);
      const std::vector<double> l = shorts(s);
      with50 = percentile(l, 0.5);
      with95 = percentile(l, 0.95);
      CHECK(waitFor(s, field, 30.0));
      std::printf("    (long job: %.2f s on %u workers)\n",
                  ms(t0, Clock::now()) / 1000.0,
                  s.threads());
    }
    std::printf("    (short jobs' latency, ms: alone p50 %.1f p95 %.1f; "
                "beside the long job p50 %.1f p95 %.1f)\n",
                alone50,
                alone95,
                with50,
                with95);
    CHECK(with50 < alone50 + 10.0);
    CHECK(with95 < alone95 + 20.0);
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
    for (int i = 0; i < 2000 && freed.load() == std::thread::id{}; ++i)
      std::this_thread::sleep_for(1ms);
    CHECK(freed.load() != std::thread::id{});
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
      CHECK(waitFor(s, id));
      s.collect(id);
    }
    for (int i = 0; i < 2000 && jobFreed.load() == std::thread::id{}; ++i)
      std::this_thread::sleep_for(1ms);
    CHECK(jobFreed.load() != std::thread::id{});
    CHECK(jobFreed.load() != std::this_thread::get_id());
  }

  return testing::report("schedulerTests");
}
