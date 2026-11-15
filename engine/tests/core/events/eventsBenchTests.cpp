#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>
#include <engine/core/events/events.h>
#include <engine/core/util/allocationMetrics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The event queues' benchmarks (docs/core/events), meant for a Release build.
// Each case prints `bench <case> <count> <ns>` and is the best of its rounds.
// The speed gates are always printed, but checked only when SFS_BENCH_GATES
// is set (and never in an unoptimised build), so a loaded machine can't fail
// the test run:
//
// - send: 10,000 sends of a 32-byte trivially copyable event a round, in ns
//   per send, against the same events pushed onto a component's vector found
//   through singleton().
// - read: 10,000 kept events read by 1 and by 4 readers, in ns per event per
//   reader.
// - update: one frame boundary with 32 idle queues, and of a queue keeping
//   10 and 10,000 trivially destructible events in each buffer, in ns per
//   update.
// - Plain vectors' push_back and loops over the same events, for context.
// - sender: 10,000 EventSender sends a round from 1 thread and from each of 4
//   at once, in ns per send, and the frame boundary that takes them in, in ns
//   per event.
// - traffic: 1,000 frames of steady sends, worker sends and reads after the
//   warm-up: the allocations they make (a static build counts them).

using namespace sfs;

namespace
{

volatile uint64_t g_sink = 0;

constexpr int kEvents = 10000;
constexpr int kRounds = 200;

// A 32-byte trivially copyable event.
struct Tick
{
  uint64_t id = 0;
  float values[6] = {};
};
static_assert(sizeof(Tick) == 32);

// The component a request list lives on today (an account's requests).
struct Ledger
{
  std::vector<Tick> requests;
};

template <int N>
struct Idle
{
  int value = N;
};

using Clock = std::chrono::steady_clock;

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

double benchSend()
{
  Events events;
  EventReader<Tick> reader = events.reader<Tick>();
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
      events.send(Tick{static_cast<uint64_t>(i), {}});
    best = std::min(best, nsSince(start) / kEvents);
    g_sink = g_sink + reader.unread();
    events.update();
    events.update();
  }
  return best;
}

/**
 * Sends events built beforehand, so the time is the send itself and not the
 * copy of a freshly built temporary.
 * @return the best nanoseconds per send over the rounds
 */
double benchSendBuilt()
{
  Events events;
  EventReader<Tick> reader = events.reader<Tick>();
  std::vector<Tick> built;
  built.reserve(kEvents);
  for (int i = 0; i < kEvents; ++i)
    built.push_back(Tick{static_cast<uint64_t>(i), {}});
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (const Tick& tick : built)
      events.send(tick);
    best = std::min(best, nsSince(start) / kEvents);
    g_sink = g_sink + reader.unread();
    events.update();
    events.update();
  }
  return best;
}

double benchPushBack()
{
  Registry registry;
  Entity owner = registry.createEntity();
  owner.addComponent<Ledger>();
  registry.flushEntities();
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
      if (Ledger* ledger = registry.singleton<Ledger>())
        ledger->requests.push_back(Tick{static_cast<uint64_t>(i), {}});
    best = std::min(best, nsSince(start) / kEvents);
    Ledger* ledger = registry.singleton<Ledger>();
    g_sink = g_sink + ledger->requests.size();
    ledger->requests.clear();
  }
  return best;
}

// The same events pushed onto a plain vector with room for them: what a
// send's push_back costs alone, printed for context.
double benchVectorPush()
{
  std::vector<Tick> ticks;
  ticks.reserve(kEvents);
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
      ticks.push_back(Tick{static_cast<uint64_t>(i), {}});
    best = std::min(best, nsSince(start) / kEvents);
    g_sink = g_sink + ticks.size();
    ticks.clear();
  }
  return best;
}

// The same reads from a plain vector, `readers` times: what going through
// the events costs alone, printed for context.
double benchVectorRead(int readers)
{
  std::vector<Tick> ticks;
  for (int i = 0; i < kEvents; ++i)
    ticks.push_back(Tick{static_cast<uint64_t>(i), {}});
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (int r = 0; r < readers; ++r)
      for (const Tick& tick : ticks)
        sum += tick.id;
    best = std::min(best, nsSince(start) / (kEvents * readers));
    g_sink = g_sink + sum;
  }
  return best;
}

double benchRead(int readers)
{
  Events events;
  for (int i = 0; i < kEvents; ++i)
    events.send(Tick{static_cast<uint64_t>(i), {}});
  double best = std::numeric_limits<double>::max();
  std::vector<EventReader<Tick>> made(static_cast<std::size_t>(readers));
  for (int round = 0; round < kRounds; ++round)
  {
    for (EventReader<Tick>& reader : made)
      reader = events.reader<Tick>();
    uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (EventReader<Tick>& reader : made)
      for (const Tick& tick : reader.read())
        sum += tick.id;
    best = std::min(best, nsSince(start) / (kEvents * readers));
    g_sink = g_sink + sum;
  }
  return best;
}

template <int... Ns>
void makeIdle(Events& events, std::integer_sequence<int, Ns...>)
{
  (events.reader<Idle<Ns>>(), ...);
}

double benchIdleUpdate()
{
  Events events;
  makeIdle(events, std::make_integer_sequence<int, 32>());
  constexpr int repeats = 1000;
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < repeats; ++i)
      events.update();
    best = std::min(best, nsSince(start) / repeats);
  }
  return best;
}

// A small trivially destructible event, so many queues of 10,000 fit in
// memory.
struct Mark
{
  uint32_t id = 0;
};

// One update of a queue keeping 10 and one keeping 10,000 events in each of
// its buffers, in ns. A single update is too short for the clock, so 256
// queues, each in an Events of its own, are updated in a row, both counts
// in every round (with their headers in cache, as in a game), and the rounds'
// times are summed.
std::pair<double, double> benchKeptUpdate()
{
  constexpr int kQueues = 256;
  struct Set
  {
    int kept = 0;
    std::vector<std::unique_ptr<Events>> all;
    double total = 0.0;
  };
  Set sets[2] = {{10, {}, 0.0}, {kEvents, {}, 0.0}};
  for (Set& set : sets)
    for (int q = 0; q < kQueues; ++q)
      set.all.push_back(std::make_unique<Events>());
  std::vector<EventStats> stats;
  for (int round = 0; round < kRounds; ++round)
    for (Set& set : sets)
    {
      for (const std::unique_ptr<Events>& events : set.all)
      {
        for (int i = 0; i < set.kept; ++i)
          events->send(Mark{static_cast<uint32_t>(i)});
        events->update();
        for (int i = 0; i < set.kept; ++i)
          events->send(Mark{static_cast<uint32_t>(i)});
      }
      for (const std::unique_ptr<Events>& events : set.all)
        events->stats(stats);
      const Clock::time_point start = Clock::now();
      for (const std::unique_ptr<Events>& events : set.all)
        events->update();
      set.total += nsSince(start);
      for (const std::unique_ptr<Events>& events : set.all)
        events->update();
    }
  return {
      sets[0].total / (kRounds * kQueues), sets[1].total / (kRounds * kQueues)};
}

// Each of `threads` threads sends kEvents through its copy of one sender,
// all at once: the best round's ns per send, as each thread sees it. The
// frame boundary that takes them in is timed too, in ns per event.
std::pair<double, double> benchSender(int threads)
{
  Events events;
  EventReader<Tick> reader = events.reader<Tick>();
  const EventSender<Tick> sender = events.sender<Tick>();
  double best = std::numeric_limits<double>::max();
  double bestDrain = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds / 4; ++round)
  {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<double> took(static_cast<std::size_t>(threads), 0.0);
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
      workers.emplace_back(
          [&, t, own = sender]() mutable
          {
            ready.fetch_add(1);
            while (!go.load())
            {
            }
            const Clock::time_point start = Clock::now();
            for (int i = 0; i < kEvents; ++i)
              own.send(Tick{static_cast<uint64_t>(i), {}});
            took[static_cast<std::size_t>(t)] = nsSince(start) / kEvents;
          });
    while (ready.load() < threads)
    {
    }
    go.store(true);
    for (std::thread& worker : workers)
      worker.join();
    best = std::min(best, *std::max_element(took.begin(), took.end()));

    const Clock::time_point start = Clock::now();
    events.update();
    bestDrain = std::min(bestDrain, nsSince(start) / (kEvents * threads));
    g_sink = g_sink + reader.unread();
    reader.skip();
    events.update();
  }
  return {best, bestDrain};
}

// Allocations made by 1,000 frames of steady traffic after the warm-up; -1
// when this build doesn't count them.
long long benchTraffic()
{
  MemoryMetrics& metrics = getMemoryMetrics();
  const std::size_t probe = metrics.allocationCount.load();
  {
    ScopedMemoryTracking track(MemoryTrackingPhase::Update);
    int* volatile block = new int(1);
    delete block;
  }
  if (metrics.allocationCount.load() == probe)
    return -1;

  Events events;
  EventReader<Tick> first = events.reader<Tick>();
  EventReader<Tick> second = events.reader<Tick>();
  EventSender<Tick> sender = events.sender<Tick>();
  std::vector<EventStats> stats;
  uint64_t sum = 0;
  const auto frame = [&]
  {
    events.update();
    for (int i = 0; i < 256; ++i)
      events.send(Tick{static_cast<uint64_t>(i), {}});
    for (int i = 0; i < 16; ++i)
      sender.send(Tick{static_cast<uint64_t>(i), {}});
    for (const Tick& tick : first.read())
      sum += tick.id;
    for (const Tick& tick : second.read())
      sum += tick.id;
    events.stats(stats);
  };
  // Until both buffers (and the inbox's two vectors) have grown to a frame's
  // events, workers' included.
  for (int i = 0; i < 4; ++i)
    frame();
  const std::size_t before = metrics.allocationCount.load();
  {
    ScopedMemoryTracking track(MemoryTrackingPhase::Update);
    for (int i = 0; i < 1000; ++i)
      frame();
  }
  g_sink = g_sink + sum;
  return static_cast<long long>(metrics.allocationCount.load() - before);
}

} // namespace

int main()
{
  if (!checkGates())
    std::printf("  the timing gates are printed, not checked (set "
                "SFS_BENCH_GATES in an optimised build to check them)\n");

  TEST("sending should cost no more than pushing onto a component's list")
  {
    const double built = benchSendBuilt();
    const double send = benchSend();
    const double push = benchPushBack();
    print("send-built", kEvents, built);
    print("send", kEvents, send);
    print("component-push-back", kEvents, push);
    const double vector = benchVectorPush();
    print("vector-push-back", kEvents, vector);
    // Sending a freshly built temporary also pays for copying it through a
    // store-forwarding stall, as a plain push_back of one does, so the 4 ns
    // target is gated on events built beforehand.
    gate("send of a built event <= 4 ns", built, 4.0);
    gate("send <= 1.5 x a component's push_back", send, 1.5 * push);
  }

  TEST("reading should cost under half a nanosecond an event")
  {
    const double one = benchRead(1);
    const double four = benchRead(4);
    print("read-1", kEvents, one);
    print("read-4", kEvents, four);
    const double vectorOne = benchVectorRead(1);
    const double vectorFour = benchVectorRead(4);
    print("vector-read-1", kEvents, vectorOne);
    print("vector-read-4", kEvents, vectorFour);
    // Where a plain vector's loop alone costs more, the events' may cost 10 %
    // more than it.
    gate("read, 1 reader <= 0.5 ns, or a vector's loop + 10%",
         one,
         std::max(0.5, 1.1 * vectorOne));
    gate("read, 4 readers <= 0.5 ns, or a vector's loop + 10%",
         four,
         std::max(0.5, 1.1 * vectorFour));
  }

  TEST("a frame boundary should cost little, whatever is kept")
  {
    const double idle = benchIdleUpdate();
    const auto [few, many] = benchKeptUpdate();
    print("update-32-idle", 32, idle);
    print("update-kept", 10, few);
    print("update-kept", kEvents, many);
    gate("update, 32 idle queues <= 100 ns", idle, 100.0);
    gate("update, 10,000 kept <= 10 kept + 5% (or + 2 ns)",
         many,
         std::max(1.05 * few, few + 2.0));
  }

  TEST("senders should cost little, alone and contended")
  {
    const auto [one, oneDrain] = benchSender(1);
    const auto [four, fourDrain] = benchSender(4);
    print("sender-1", kEvents, one);
    print("sender-4", kEvents, four);
    print("drain", kEvents, oneDrain);
    print("drain", 4 * kEvents, fourDrain);
    gate("EventSender::send, 1 thread <= 50 ns", one, 50.0);
    gate("EventSender::send, 4 contending <= 250 ns", four, 250.0);
    gate("drain <= 3 ns per event", std::max(oneDrain, fourDrain), 3.0);
  }

  TEST("steady traffic should allocate nothing once warm")
  {
    const long long allocations = benchTraffic();
    if (allocations < 0)
      std::printf("  traffic: allocation tracking is off in this build (a "
                  "static build counts it)\n");
    else
    {
      print("traffic-allocations", 1000, static_cast<double>(allocations));
      CHECK(allocations == 0);
    }
  }

  return testing::report("eventsBenchTests");
}
