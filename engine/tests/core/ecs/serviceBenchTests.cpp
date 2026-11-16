#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>
#include <engine/core/events/events.h>
#include <engine/core/util/allocationMetrics.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Services' benchmarks (docs/core/services), meant for a Release build. Each
// case prints `bench <case> <count> <ns>` and is the best of its rounds. The
// gates are always printed, but checked only when SFS_BENCH_GATES is set (and
// never in an unoptimised build), so a loaded machine can't fail the run:
//
// - idle: runServices() over 16 services with 4 handles each and nothing new,
//   in ns per handle.
// - woken: a service handling 1,000 news events with onEvent(), against the
//   same handler run through a System's updateSystem(), in ns per run.
// - swap: a task swap per event (remove one component, add another, on the
//   handed entity, a system following both lists), against the same swap in
//   a System's update over the same events, in ns per run.
// - allocation: 1,000 steps of steady traffic through 4 services after
//   warm-up allocate nothing (counted in a static build).

using namespace sfs;

namespace
{

struct [[sfs::event_news]] IdleEvent
{
  int value = 0;
};

struct [[sfs::event_news]] WorkEvent
{
  int value = 0;
};

struct [[sfs::event_entity_news]] SwapEvent
{
  int value = 0;
};

struct TaskA
{
  int value = 0;
};

struct TaskB
{
  int value = 0;
};

constexpr int kRounds = 7;
constexpr int kEvents = 1000;

using Clock = std::chrono::steady_clock;

volatile int64_t g_sink = 0;

double nsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

void print(const char* name, int count, double ns)
{
  std::printf("bench %s %d %.3f\n", name, count, ns);
}

// Whether the gates are checked: asked for, in an optimised build.
bool checkGates()
{
#ifdef NDEBUG
  static const bool asked = std::getenv("SFS_BENCH_GATES") != nullptr;
  return asked;
#else
  return false;
#endif
}

// One gate, `now` at most `limit`: printed, and checked when checkGates().
void gate(const std::string& what, double now, double limit)
{
  const bool met = now <= limit;
  std::printf("  gate %-58s: %.3f against %.3f: %s\n",
              what.c_str(),
              now,
              limit,
              met ? "met" : "MISSED");
  if (checkGates())
    CHECK(met);
}

// Whether allocations are counted in this build (a static engine build).
bool tracking()
{
  MemoryMetrics& metrics = getMemoryMetrics();
  const std::size_t probe = metrics.allocationCount.load();
  {
    ScopedMemoryTracking track(MemoryTrackingPhase::Update);
    int* volatile block = new int(1);
    delete block;
  }
  return metrics.allocationCount.load() != probe;
}

// A service whose create() runs what a bench gives it.
class Host : public Service
{
public:
  explicit Host(std::function<void(Host&)> setUp) : m_setUp(std::move(setUp)) {}

  using Service::consumer;

protected:
  void create() override { m_setUp(*this); }

private:
  std::function<void(Host&)> m_setUp;
};

// A system whose create() and update() run what a bench gives them.
class Runner : public System
{
public:
  explicit Runner(std::function<void(Runner&)> setUp)
      : m_setUp(std::move(setUp))
  {
  }

  using EventVerbs::consumer;
  using System::observe;

  std::function<void(Runner&)> body;

protected:
  void create() override { m_setUp(*this); }
  void update(double) override
  {
    if (body)
      body(*this);
  }

private:
  std::function<void(Runner&)> m_setUp;
};

double benchIdle()
{
  Registry registry;
  std::vector<std::unique_ptr<EventConsumer<IdleEvent>>> handles;
  for (int s = 0; s < 16; ++s)
    registry.addService<Host>(
        [&](Host& h)
        {
          for (int c = 0; c < 4; ++c)
            handles.push_back(std::make_unique<EventConsumer<IdleEvent>>(
                h.consumer<IdleEvent>().onEvent(
                    [](const IdleEvent& e) { g_sink = g_sink + e.value; })));
        });
  constexpr int kRuns = 100000;
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kRuns; ++i)
      registry.runServices();
    best = std::min(best, nsSince(start) / (kRuns * 64.0));
  }
  handles.clear();
  return best;
}

// One run handling kEvents news events with onEvent(): through a service
// (service true) or a system's updateSystem().
double benchWoken(bool service)
{
  Registry registry;
  EventConsumer<WorkEvent> handle;
  int64_t sum = 0;
  const auto handler = [&sum](const WorkEvent& e) { sum += e.value; };
  Runner* runner = nullptr;
  if (service)
    registry.addService<Host>(
        [&](Host& h) { handle = h.consumer<WorkEvent>().onEvent(handler); });
  else
    runner = &registry.addSystem<Runner>(
        [&](Runner& r) { handle = r.consumer<WorkEvent>().onEvent(handler); });
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds * 4; ++round)
  {
    for (int i = 0; i < kEvents; ++i)
      registry.events().send(WorkEvent{i});
    const Clock::time_point start = Clock::now();
    if (service)
      registry.runServices();
    else
      registry.updateSystem(*runner, 0.0);
    best = std::min(best, nsSince(start));
    registry.events().update();
  }
  g_sink = g_sink + sum;
  return best;
}

// One run swapping TaskA and TaskB on kEvents entities, one per event: in a
// service's handler (service true) or a system's update over the same
// events; a system follows both lists either way.
double benchSwap(bool service)
{
  Registry registry;
  EventConsumer<SwapEvent> handle;
  int joined = 0;
  const auto swap = [](const Entity& entity)
  {
    if (entity.hasComponent<TaskA>())
    {
      entity.removeComponent<TaskA>();
      entity.addComponent<TaskB>();
    }
    else
    {
      entity.removeComponent<TaskB>();
      entity.addComponent<TaskA>();
    }
  };
  registry.addSystem<Runner>(
      [&](Runner& r)
      {
        r.observe<TaskA>([&](const Entity&) { ++joined; });
        r.observe<TaskB>([&](const Entity&) { ++joined; });
      });
  Runner* swapper = nullptr;
  if (service)
    registry.addService<Host>(
        [&](Host& h)
        {
          handle = h.consumer<SwapEvent>().onEvent(
              [&](const Entity& entity, const SwapEvent&) { swap(entity); });
        });
  else
  {
    swapper = &registry.addSystem<Runner>(
        [&](Runner& r) { handle = r.consumer<SwapEvent>(); });
    swapper->body = [&](Runner&)
    {
      for (auto [entity, event] : handle.read())
        swap(entity);
    };
  }
  std::vector<Entity> entities;
  entities.reserve(kEvents);
  for (int i = 0; i < kEvents; ++i)
  {
    entities.push_back(registry.createEntity());
    entities.back().addComponent<TaskA>();
  }
  registry.flushEntities();
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds * 2; ++round)
  {
    for (const Entity& entity : entities)
      registry.events().send(entity, SwapEvent{1});
    const Clock::time_point start = Clock::now();
    if (service)
      registry.runServices();
    else
      registry.updateSystem(*swapper, 0.0);
    best = std::min(best, nsSince(start));
    registry.events().update();
  }
  g_sink = g_sink + joined;
  return best;
}

// Whether 1,000 steps of steady traffic through 4 services allocate nothing
// once warm.
bool steadyAllocatesNothing()
{
  Registry registry;
  std::vector<std::unique_ptr<EventConsumer<WorkEvent>>> handles;
  for (int s = 0; s < 4; ++s)
    registry.addService<Host>(
        [&](Host& h)
        {
          handles.push_back(std::make_unique<EventConsumer<WorkEvent>>(
              h.consumer<WorkEvent>().onEvent([](const WorkEvent& e)
                                              { g_sink = g_sink + e.value; })));
        });
  const auto once = [&]
  {
    for (int i = 0; i < 16; ++i)
      registry.events().send(WorkEvent{i});
    registry.flushEntities();
    registry.events().update();
    registry.runServices();
  };
  for (int i = 0; i < 8; ++i)
    once();
  MemoryMetrics& metrics = getMemoryMetrics();
  const std::size_t before = metrics.allocationCount.load();
  {
    ScopedMemoryTracking track(MemoryTrackingPhase::Update);
    for (int i = 0; i < 1000; ++i)
      once();
  }
  const bool none = metrics.allocationCount.load() == before;
  handles.clear();
  return none;
}

} // namespace

int main()
{
  if (!checkGates())
    std::printf("  the timing gates are printed, not checked (set "
                "SFS_BENCH_GATES in an optimised build to check them)\n");

  TEST("an idle service should cost a compare per handle")
  {
    const double idle = benchIdle();
    print("idle_per_handle", 64, idle);
    gate("runServices(), 16 idle services x 4 handles <= 4 ns a handle",
         idle,
         4.0);
  }

  TEST("a woken service should cost what a system's handler dispatch does")
  {
    const double system = benchWoken(false);
    const double service = benchWoken(true);
    print("woken_system", kEvents, system);
    print("woken_service", kEvents, service);
    gate("woken service <= system + 5% (or + 50 ns)",
         service,
         std::max(system * 1.05, system + 50.0));
  }

  TEST("a task swap in a service should cost what it does in a system")
  {
    const double system = benchSwap(false);
    const double service = benchSwap(true);
    print("swap_system", kEvents, system);
    print("swap_service", kEvents, service);
    gate("task swap in a service <= 1.1 x in a system", service, system * 1.1);
  }

  TEST("steady traffic through services should allocate nothing")
  {
    if (!tracking())
      std::printf("      note: allocation tracking is off in this build\n");
    else
      CHECK(steadyAllocatesNothing());
  }

  return testing::report("serviceBenchTests");
}
