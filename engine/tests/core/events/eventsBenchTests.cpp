#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>
#include <engine/core/events/events.h>
#include <engine/core/events/responder.h>
#include <engine/core/util/allocationMetrics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
//   per send, against the same events pushed onto a scene data vector found
//   through sceneData().
// - read: 10,000 kept events read by 1 and by 4 consumers, in ns per event
//   per consumer.
// - update: one frame boundary with 32 idle queues, and of a queue whose
//   consumer read the 10 or 10,000 trivially destructible events sent, in ns
//   per update; and frames with a consumer that reads every fourth, which the
//   boundary compacts, in ns per event sent.
// - Plain vectors' push_back and loops over the same events, for context.
// - broadcast: 10,000 EventBroadcast sends a round from 1 thread and from
//   each of 4 at once, in ns per send, and the frame boundary that takes them
//   in, in ns per event.
// - traffic: 1,000 frames of steady sends, worker sends, reads, handlers,
//   requests and responses after the warm-up: the allocations they make (a
//   static build counts them).
// - keyed: 10,000 keyed sends a round, to 10,000 keys and all to one key (each
//   superseding the last), and 10,000 requests from a requester, in ns per
//   send.
// - handles: 10,000 events through a read loop, an onEvent handler and an
//   onEvents one, readLatest of 10,000 keyed events, a read of 10,000
//   requests from a requester responding to each, a requester's onResponse
//   over 10,000 responses, a worker's response, and 10,000 open requests
//   answered later by entity, in ns per event.
// - family: 10,000 sends of news whose event base one consumer reads, against
//   the same sends with no consumer of the base, in ns per send.
// - requests: the memory 1,000 requesters asking about 20 entities each hold,
//   in bytes per request, and the flush of 10,000 destroyed entities with
//   1,000 requesters asking about them, against none, in ns per entity.
// - many requesters: 10 and 10,000 requesters asking about one entity (a
//   request, waiting() and cancel each), 1,000 and 10,000 of them going, and
//   100 and 4,000 requesters with handlers each answered once, in ns per
//   requester or response.

using namespace sfs;

namespace
{

// A response handler that does nothing, for requesters that only ask.
constexpr auto ignored = [](const Entity&, const auto&) {};

volatile uint64_t g_sink = 0;

constexpr int kEvents = 10000;
constexpr int kRounds = 200;

// A 32-byte trivially copyable event.
struct [[sfs::event_news]] TickEvent
{
  uint64_t id = 0;
  float values[6] = {};
};
static_assert(sizeof(TickEvent) == 32);

// The scene data a request list lives on (an account's requests).
struct [[sfs::scene_data]] Ledger
{
  std::vector<TickEvent> requests;
};

template <int N>
struct Idle
{
  int value = N;
};

} // namespace

// Codegen reads attributes only on plain structs, so these templates' traits
// are written here.
template <int N>
struct sfs::detail::EventTraits<Idle<N>>
{
  static constexpr EventKind kind = EventKind::News;
  using Ancestors = TypeList<>;
};

namespace
{

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
  EventConsumer<TickEvent> consumer = events.consumer<TickEvent>();
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
      events.send(TickEvent{static_cast<uint64_t>(i), {}});
    best = std::min(best, nsSince(start) / kEvents);
    g_sink = g_sink + consumer.unread();
    consumer.skip();
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
  EventConsumer<TickEvent> consumer = events.consumer<TickEvent>();
  std::vector<TickEvent> built;
  built.reserve(kEvents);
  for (int i = 0; i < kEvents; ++i)
    built.push_back(TickEvent{static_cast<uint64_t>(i), {}});
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (const TickEvent& tick : built)
      events.send(tick);
    best = std::min(best, nsSince(start) / kEvents);
    g_sink = g_sink + consumer.unread();
    consumer.skip();
    events.update();
  }
  return best;
}

double benchPushBack()
{
  Registry registry;
  registry.addSceneData<Ledger>();
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
      if (Ledger* ledger = registry.sceneData<Ledger>())
        ledger->requests.push_back(TickEvent{static_cast<uint64_t>(i), {}});
    best = std::min(best, nsSince(start) / kEvents);
    Ledger* ledger = registry.sceneData<Ledger>();
    g_sink = g_sink + ledger->requests.size();
    ledger->requests.clear();
  }
  return best;
}

// The same events pushed onto a plain vector with room for them: what a
// send's push_back costs alone, printed for context.
double benchVectorPush()
{
  std::vector<TickEvent> ticks;
  ticks.reserve(kEvents);
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
      ticks.push_back(TickEvent{static_cast<uint64_t>(i), {}});
    best = std::min(best, nsSince(start) / kEvents);
    g_sink = g_sink + ticks.size();
    ticks.clear();
  }
  return best;
}

// The same reads from a plain vector, `consumers` times: what going through
// the events costs alone, printed for context.
double benchVectorRead(int consumers)
{
  std::vector<TickEvent> ticks;
  for (int i = 0; i < kEvents; ++i)
    ticks.push_back(TickEvent{static_cast<uint64_t>(i), {}});
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (int r = 0; r < consumers; ++r)
      for (const TickEvent& tick : ticks)
        sum += tick.id;
    best = std::min(best, nsSince(start) / (kEvents * consumers));
    g_sink = g_sink + sum;
  }
  return best;
}

double benchRead(int consumers)
{
  Events events;
  for (int i = 0; i < kEvents; ++i)
    events.send(TickEvent{static_cast<uint64_t>(i), {}});
  double best = std::numeric_limits<double>::max();
  std::vector<EventConsumer<TickEvent>> made(
      static_cast<std::size_t>(consumers));
  for (int round = 0; round < kRounds; ++round)
  {
    for (EventConsumer<TickEvent>& consumer : made)
      consumer = events.consumer<TickEvent>();
    uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (EventConsumer<TickEvent>& consumer : made)
      for (const TickEvent& tick : consumer.read())
        sum += tick.id;
    best = std::min(best, nsSince(start) / (kEvents * consumers));
    g_sink = g_sink + sum;
  }
  return best;
}

template <int... Ns>
void makeIdle(Events& events, std::integer_sequence<int, Ns...>)
{
  (events.consumer<Idle<Ns>>(), ...);
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
struct [[sfs::event_news]] MarkEvent
{
  uint32_t id = 0;
};

// One update of a queue whose consumer has read the 10, or the 10,000, events
// sent since the last (the common case: everything consumed, so the buffer
// is cleared outright), in ns. A single update is too short for the clock, so
// 256 queues, each in an Events of its own, are updated in a row, both counts
// in every round (with their headers in cache, as in a game), and the rounds'
// times are summed.
std::pair<double, double> benchKeptUpdate()
{
  constexpr int kQueues = 256;
  struct Set
  {
    int kept = 0;
    std::vector<std::unique_ptr<Events>> all;
    std::vector<EventConsumer<MarkEvent>> consumers;
    double total = 0.0;
  };
  Set sets[2] = {{10, {}, {}, 0.0}, {kEvents, {}, {}, 0.0}};
  for (Set& set : sets)
    for (int q = 0; q < kQueues; ++q)
    {
      set.all.push_back(std::make_unique<Events>());
      set.consumers.push_back(set.all.back()->consumer<MarkEvent>());
    }
  std::vector<EventStats> stats;
  for (int round = 0; round < kRounds; ++round)
    for (Set& set : sets)
    {
      for (const std::unique_ptr<Events>& events : set.all)
        for (int i = 0; i < set.kept; ++i)
          events->send(MarkEvent{static_cast<uint32_t>(i)});
      for (EventConsumer<MarkEvent>& consumer : set.consumers)
        consumer.skip();
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

// Frames of kEvents small events with one consumer that reads them all and one
// that reads before the frame's second half is sent, so each frame boundary
// keeps half the frame's events and compacts the buffer: what it costs, in ns
// per event sent.
double benchSlowReader()
{
  Events events;
  EventConsumer<MarkEvent> fast = events.consumer<MarkEvent>();
  EventConsumer<MarkEvent> slow = events.consumer<MarkEvent>();
  constexpr int kFrames = 64;
  double best = std::numeric_limits<double>::max();
  uint64_t sum = 0;
  for (int round = 0; round < kRounds / 8; ++round)
  {
    double total = 0.0;
    for (int frame = 0; frame < kFrames; ++frame)
    {
      for (int i = 0; i < kEvents / 2; ++i)
        events.send(MarkEvent{static_cast<uint32_t>(i)});
      sum += slow.read().size();
      for (int i = 0; i < kEvents / 2; ++i)
        events.send(MarkEvent{static_cast<uint32_t>(i)});
      sum += fast.read().size();
      const Clock::time_point start = Clock::now();
      events.update();
      total += nsSince(start);
    }
    best = std::min(best, total / (kFrames * kEvents));
  }
  g_sink = g_sink + sum;
  return best;
}

// Each of `threads` threads sends kEvents through its copy of one broadcast,
// all at once: the best round's ns per send, as each thread sees it. The
// frame boundary that takes them in is timed too, in ns per event.
std::pair<double, double> benchBroadcast(int threads)
{
  Events events;
  EventConsumer<TickEvent> consumer = events.consumer<TickEvent>();
  const EventBroadcast<TickEvent> broadcast = events.broadcast<TickEvent>();
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
          [&, t, own = broadcast]() mutable
          {
            ready.fetch_add(1);
            while (!go.load())
            {
            }
            const Clock::time_point start = Clock::now();
            for (int i = 0; i < kEvents; ++i)
              own.send(TickEvent{static_cast<uint64_t>(i), {}});
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
    g_sink = g_sink + consumer.unread();
    consumer.skip();
    events.update();
  }
  return {best, bestDrain};
}

// A keyed event, the size of TickEvent.
struct [[sfs::event_entity_news]] KeyedTickEvent
{
  uint64_t id = 0;
  float values[6] = {};
};

// A request and its response.
struct [[sfs::event_response]] ToldResponse
{
  uint64_t id = 0;
};
struct [[sfs::event_request(ToldResponse)]] AskRequest
{
  uint64_t id = 0;
};

// A request responded to from a worker, and its response.
struct [[sfs::event_response]] WorkToldResponse
{
  uint64_t id = 0;
};
struct [[sfs::event_request(WorkToldResponse)]] WorkAskRequest
{
  uint64_t id = 0;
};

// News for the handler bench, read by a loop and by a handler.
struct [[sfs::event_news]] HandledEvent
{
  uint64_t id = 0;
  float values[6] = {};
};

// News and the news it derives from, for the family send bench.
struct [[sfs::event_news]] DamageEvent
{
  uint64_t id = 0;
  float amount = 0.0f;
};
struct [[sfs::event_news]] FireDamageEvent : DamageEvent
{
  float burnSeconds = 0.0f;
  float values[4] = {};
};

// A system whose create() makes its handles and whose update runs what a
// bench gives it.
class Bencher : public System
{
public:
  /** @param setUp what it does in create(): make its handles */
  explicit Bencher(std::function<void(Bencher&)> setUp)
      : m_setUp(std::move(setUp))
  {
  }

  using EventVerbs::consumer;
  using EventVerbs::requester;

  std::function<void(Bencher&)> body = [](Bencher&) {};

protected:
  void create() override { m_setUp(*this); }
  void update(double) override { body(*this); }

private:
  std::function<void(Bencher&)> m_setUp;
};

// A registry with `count` flushed entities.
std::vector<Entity> makeEntities(Registry& registry, int count)
{
  std::vector<Entity> made;
  made.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i)
    made.push_back(registry.createEntity());
  registry.flushEntities();
  return made;
}

// 10,000 keyed sends a round, to as many keys or all to the first: the best
// round's ns per send.
double benchKeyedSend(bool oneKey)
{
  Registry registry;
  const std::vector<Entity> keys = makeEntities(registry, kEvents);
  Events& events = registry.events();
  KeyedTickEvent tick;
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
    {
      tick.id = static_cast<uint64_t>(i);
      events.send(oneKey ? keys[0] : keys[static_cast<std::size_t>(i)], tick);
    }
    best = std::min(best, nsSince(start) / kEvents);
    events.update();
    events.update();
  }
  return best;
}

// 10,000 requests a round from one requester, to as many entities: the best
// round's ns per request (its ticket, then the append).
double benchRequest()
{
  Registry registry;
  const std::vector<Entity> keys = makeEntities(registry, kEvents);
  EventRequester<AskRequest> asker;
  registry.addSystem<Bencher>(
      [&](Bencher& self)
      { asker = self.requester<AskRequest>().onResponse(ignored); });
  Events& events = registry.events();
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
      asker.request(keys[static_cast<std::size_t>(i)],
                    AskRequest{static_cast<uint64_t>(i)});
    best = std::min(best, nsSince(start) / kEvents);
    events.update();
    events.update();
  }
  return best;
}

// A system holding many requesters, each asking about its own entities.
class Askers : public System
{
public:
  explicit Askers(int count) : m_count(count) {}
  std::vector<EventRequester<AskRequest>> askers;

protected:
  void create() override
  {
    askers.reserve(static_cast<std::size_t>(m_count));
    for (int i = 0; i < m_count; ++i)
      askers.push_back(requester<AskRequest>().onResponse(ignored));
  }

private:
  int m_count;
};

// `count` requesters asking about one entity: the best round's ns per
// request, waiting() and cancel, each requester once a round.
double benchFanIn(int count)
{
  Registry registry;
  const Entity target = makeEntities(registry, 1).front();
  Askers& askers = registry.addSystem<Askers>(count);
  double best = std::numeric_limits<double>::max();
  uint64_t sum = 0;
  for (int round = 0; round < 20; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (EventRequester<AskRequest>& asker : askers.askers)
      asker.request(target, AskRequest{1});
    for (EventRequester<AskRequest>& asker : askers.askers)
      sum += asker.waiting(target) ? 1u : 0u;
    for (EventRequester<AskRequest>& asker : askers.askers)
      asker.cancel(target);
    best = std::min(best, nsSince(start) / (3.0 * count));
    registry.events().update();
    registry.events().update();
  }
  g_sink = g_sink + sum;
  return best;
}

// Destroying `count` requesters that each asked about one entity: ns per
// requester.
double benchRequestersGoing(int count)
{
  Registry registry;
  const Entity target = makeEntities(registry, 1).front();
  Askers& askers = registry.addSystem<Askers>(count);
  for (EventRequester<AskRequest>& asker : askers.askers)
    asker.request(target, AskRequest{1});
  const Clock::time_point start = Clock::now();
  askers.askers.clear();
  return nsSince(start) / count;
}

// A system with `count` requesters, each handling its responses.
class Listeners : public System
{
public:
  explicit Listeners(int count) : m_count(count) {}
  std::vector<EventRequester<AskRequest>> askers;
  uint64_t heard = 0;

protected:
  void create() override
  {
    askers.reserve(static_cast<std::size_t>(m_count));
    for (int i = 0; i < m_count; ++i)
      askers.push_back(requester<AskRequest>().onResponse(
          [this](const Entity&, const ToldResponse& told)
          { heard += told.id; }));
  }

private:
  int m_count;
};

// `count` requesters each asking about its own entity, each answered once a
// round: the best round's ns per response for the requesters' update (their
// handlers).
double benchManyRequesters(int count)
{
  Registry registry;
  const std::vector<Entity> keys = makeEntities(registry, count);
  EventConsumer<AskRequest> asks;
  Bencher& server = registry.addSystem<Bencher>(
      [&](Bencher& self) { asks = self.consumer<AskRequest>(); });
  Listeners& listeners = registry.addSystem<Listeners>(count);
  server.body = [&](Bencher&)
  {
    for (auto [key, ask, responder] : asks.read())
      responder.respond(ToldResponse{ask.id});
  };
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < 20; ++round)
  {
    for (int i = 0; i < count; ++i)
      listeners.askers[static_cast<std::size_t>(i)].request(
          keys[static_cast<std::size_t>(i)],
          AskRequest{static_cast<uint64_t>(i)});
    registry.updateSystem(server, 0.0);
    const Clock::time_point start = Clock::now();
    registry.updateSystem(listeners, 0.0);
    best = std::min(best, nsSince(start) / count);
    registry.events().update();
    registry.events().update();
  }
  g_sink = g_sink + listeners.heard;
  return best;
}

// The memory a request type holds with 1,000 requesters each asking about
// 20 entities, in bytes per outstanding request.
double benchRequesterMemory()
{
  Registry registry;
  const std::vector<Entity> keys = makeEntities(registry, 20000);
  Askers& askers = registry.addSystem<Askers>(1000);
  for (std::size_t a = 0; a < askers.askers.size(); ++a)
    for (std::size_t e = 0; e < 20; ++e)
      askers.askers[a].request(keys[a * 20 + e], AskRequest{e});
  std::vector<EventStats> stats;
  registry.events().stats(stats);
  std::size_t bytes = 0;
  for (const EventStats& type : stats)
    bytes += type.requestBytes;
  std::printf(
      "  requests: 1,000 requesters x 20 entities hold %zu bytes\n", bytes);
  return static_cast<double>(bytes) / 20000.0;
}

// Destroying 10,000 entities and the flush that supersedes their requests,
// with 1,000 requesters each asking about 10 of them (or none): the best
// round's ns per entity.
double benchRequesterFlush(bool requesters)
{
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < 20; ++round)
  {
    Registry registry;
    const std::vector<Entity> keys = makeEntities(registry, kEvents);
    Askers& askers = registry.addSystem<Askers>(requesters ? 1000 : 0);
    for (std::size_t a = 0; a < askers.askers.size(); ++a)
      for (std::size_t e = 0; e < 10; ++e)
        askers.askers[a].request(keys[a * 10 + e], AskRequest{e});
    for (const Entity& key : keys)
      registry.destroyEntity(key);
    const Clock::time_point start = Clock::now();
    registry.flushEntities();
    best = std::min(best, nsSince(start) / kEvents);
  }
  return best;
}

// 10,000 sends a round of news with an event base that one consumer reads
// (or, `plain`, that nobody consumes, so nothing is copied): the best round's
// ns per send.
double benchFamilySend(bool plain)
{
  Registry registry;
  EventConsumer<DamageEvent> damage;
  if (!plain)
    registry.addSystem<Bencher>([&](Bencher& self)
                                { damage = self.consumer<DamageEvent>(); });
  Events& events = registry.events();
  FireDamageEvent fire;
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
    {
      fire.id = static_cast<uint64_t>(i);
      events.send(fire);
    }
    best = std::min(best, nsSince(start) / kEvents);
    damage.skip();
    events.update();
    events.update();
  }
  return best;
}

// Times one update of a system, its handlers included, that receives kEvents
// events `fill` sends first each round: the best round's ns per event.
double benchUpdate(const std::function<void(Bencher&)>& setUp,
                   const std::function<void(Registry&)>& fill,
                   const std::function<uint64_t()>& consume = {})
{
  Registry registry;
  Bencher& bencher = registry.addSystem<Bencher>(setUp);
  uint64_t sum = 0;
  if (consume)
    bencher.body = [&](Bencher&) { sum += consume(); };
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    fill(registry);
    const Clock::time_point start = Clock::now();
    registry.updateSystem(bencher, 0.0);
    best = std::min(best, nsSince(start) / kEvents);
    registry.events().update();
    registry.events().update();
  }
  g_sink = g_sink + sum;
  return best;
}

void sendHandled(Registry& registry)
{
  for (int i = 0; i < kEvents; ++i)
    registry.events().send(HandledEvent{static_cast<uint64_t>(i), {}});
}

// kEvents events read by a loop in update().
double benchReadLoop()
{
  EventConsumer<HandledEvent> handled;
  return benchUpdate([&](Bencher& self)
                     { handled = self.consumer<HandledEvent>(); },
                     sendHandled,
                     [&]
                     {
                       uint64_t sum = 0;
                       for (const HandledEvent& event : handled.read())
                         sum += event.id;
                       return sum;
                     });
}

// The same events through an onEvent handler, or a batch onEvents one.
double benchHandler(bool batch)
{
  EventConsumer<HandledEvent> handled;
  uint64_t sum = 0;
  const double best = benchUpdate(
      [&](Bencher& self)
      {
        if (batch)
          handled = self.consumer<HandledEvent>().onEvents(
              [&](EventRange<HandledEvent> events)
              {
                for (const HandledEvent& event : events)
                  sum += event.id;
              });
        else
          handled = self.consumer<HandledEvent>().onEvent(
              [&](const HandledEvent& event) { sum += event.id; });
      },
      sendHandled);
  g_sink = g_sink + sum;
  return best;
}

// readLatest of kEvents keyed events, one per key.
double benchLatest()
{
  EventConsumer<KeyedTickEvent> ticks;
  std::vector<Entity> keys;
  return benchUpdate(
      [&](Bencher& self) { ticks = self.consumer<KeyedTickEvent>(); },
      [&](Registry& registry)
      {
        if (keys.empty())
          keys = makeEntities(registry, kEvents);
        for (int i = 0; i < kEvents; ++i)
          registry.events().send(keys[static_cast<std::size_t>(i)],
                                 KeyedTickEvent{static_cast<uint64_t>(i), {}});
      },
      [&]
      {
        uint64_t sum = 0;
        for (auto [key, tick] : ticks.readLatest())
          sum += tick.id;
        return sum;
      });
}

// A read of kEvents requests from a requester, one per key, each responded
// to at once.
double benchResponders()
{
  EventConsumer<AskRequest> asks;
  EventRequester<AskRequest> asker;
  std::vector<Entity> keys;
  return benchUpdate(
      [&](Bencher& self)
      {
        asks = self.consumer<AskRequest>();
        asker = self.requester<AskRequest>().onResponse(ignored);
      },
      [&](Registry& registry)
      {
        if (keys.empty())
          keys = makeEntities(registry, kEvents);
        for (int i = 0; i < kEvents; ++i)
          asker.request(keys[static_cast<std::size_t>(i)],
                        AskRequest{static_cast<uint64_t>(i)});
      },
      [&]
      {
        uint64_t sum = 0;
        for (auto [key, ask, responder] : asks.read())
          sum += responder.respond(ToldResponse{ask.id}) ? 1u : 0u;
        return sum;
      });
}

// A server answering kEvents open requests later by entity, one per key:
// the best round's ns per response.
double benchRespondByEntity()
{
  Registry registry;
  const std::vector<Entity> keys = makeEntities(registry, kEvents);
  EventConsumer<AskRequest> asks;
  EventRequester<AskRequest> asker;
  Bencher& system = registry.addSystem<Bencher>(
      [&](Bencher& self)
      {
        asks = self.consumer<AskRequest>();
        asker = self.requester<AskRequest>().onResponse(ignored);
      });
  double best = std::numeric_limits<double>::max();
  uint64_t sum = 0;
  for (int round = 0; round < kRounds; ++round)
  {
    for (int i = 0; i < kEvents; ++i)
      asker.request(keys[static_cast<std::size_t>(i)],
                    AskRequest{static_cast<uint64_t>(i)});
    asks.skip();
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kEvents; ++i)
      sum += asks.respond(keys[static_cast<std::size_t>(i)],
                          ToldResponse{static_cast<uint64_t>(i)})
                 ? 1u
                 : 0u;
    best = std::min(best, nsSince(start) / kEvents);
    // The asker hears its responses, so nothing backs up.
    registry.updateSystem(system, 0.0);
    registry.events().update();
    registry.events().update();
  }
  g_sink = g_sink + sum;
  return best;
}

// A requester's onResponse handler over kEvents responses, one per key.
double benchResponses()
{
  Registry registry;
  const std::vector<Entity> keys = makeEntities(registry, kEvents);
  EventConsumer<AskRequest> asks;
  EventRequester<AskRequest> asker;
  uint64_t sum = 0;
  Bencher& server = registry.addSystem<Bencher>(
      [&](Bencher& self) { asks = self.consumer<AskRequest>(); });
  Bencher& client = registry.addSystem<Bencher>(
      [&](Bencher& self)
      {
        asker = self.requester<AskRequest>().onResponse(
            [&](const Entity&, const ToldResponse& told) { sum += told.id; });
      });
  server.body = [&](Bencher&)
  {
    for (auto [key, ask, responder] : asks.read())
      responder.respond(ToldResponse{ask.id});
  };
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds; ++round)
  {
    for (int i = 0; i < kEvents; ++i)
      asker.request(keys[static_cast<std::size_t>(i)],
                    AskRequest{static_cast<uint64_t>(i)});
    registry.updateSystem(server, 0.0);
    const Clock::time_point start = Clock::now();
    registry.updateSystem(client, 0.0);
    best = std::min(best, nsSince(start) / kEvents);
    registry.events().update();
    registry.events().update();
  }
  g_sink = g_sink + sum;
  return best;
}

// A worker's response to each of kEvents requests, as the worker sees it:
// ns per response.
double benchWorkerResponse()
{
  Registry registry;
  const std::vector<Entity> keys = makeEntities(registry, kEvents);
  EventConsumer<WorkAskRequest> asks;
  EventRequester<WorkAskRequest> asker;
  Bencher& server = registry.addSystem<Bencher>(
      [&](Bencher& self) { asks = self.consumer<WorkAskRequest>(); });
  Bencher& client = registry.addSystem<Bencher>(
      [&](Bencher& self)
      { asker = self.requester<WorkAskRequest>().onResponse(ignored); });
  std::vector<Responder<WorkAskRequest>> responders;
  responders.reserve(kEvents);
  server.body = [&](Bencher&)
  {
    for (auto [key, ask, responder] : asks.read())
      responders.push_back(std::move(responder));
  };
  double best = std::numeric_limits<double>::max();
  for (int round = 0; round < kRounds / 4; ++round)
  {
    for (int i = 0; i < kEvents; ++i)
      asker.request(keys[static_cast<std::size_t>(i)],
                    WorkAskRequest{static_cast<uint64_t>(i)});
    registry.updateSystem(server, 0.0);
    double took = 0.0;
    std::thread worker(
        [&]
        {
          const Clock::time_point start = Clock::now();
          for (const Responder<WorkAskRequest>& responder : responders)
            responder.respond(WorkToldResponse{responder.key().getId()});
          took = nsSince(start);
        });
    worker.join();
    best = std::min(best, took / kEvents);
    registry.events().takeIn();
    responders.clear();
    registry.updateSystem(client, 0.0);
    registry.events().update();
    registry.events().update();
  }
  return best;
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

  uint64_t sum = 0;
  Registry registry;
  Events& events = registry.events();
  EventConsumer<TickEvent> first = events.consumer<TickEvent>();
  EventConsumer<TickEvent> second = events.consumer<TickEvent>();
  EventBroadcast<TickEvent> broadcast = events.broadcast<TickEvent>();
  // The handles' share: requests responded to from the main thread and from
  // a worker, keyed news read latest, handlers and news with a base.
  const std::vector<Entity> keys = makeEntities(registry, 64);
  std::vector<Responder<WorkAskRequest>> pending;
  pending.reserve(64);
  EventConsumer<AskRequest> asks;
  EventConsumer<WorkAskRequest> workAsks;
  EventConsumer<KeyedTickEvent> ticks;
  EventConsumer<HandledEvent> handled;
  EventConsumer<DamageEvent> damage;
  EventRequester<AskRequest> asker;
  EventRequester<WorkAskRequest> workAsker;
  Bencher& bencher = registry.addSystem<Bencher>(
      [&](Bencher& self)
      {
        asks = self.consumer<AskRequest>();
        workAsks = self.consumer<WorkAskRequest>();
        ticks = self.consumer<KeyedTickEvent>();
        handled = self.consumer<HandledEvent>().onEvent(
            [&](const HandledEvent& event) { sum += event.id; });
        damage = self.consumer<DamageEvent>().onEvent(
            [&](const DamageEvent& event) { sum += event.id; });
        asker = self.requester<AskRequest>().onResponse(
            [&](const Entity&, const ToldResponse& told) { sum += told.id; });
        workAsker = self.requester<WorkAskRequest>().onResponses(
            [&](ResponseRange<WorkAskRequest, WorkToldResponse> told)
            { sum += told.size(); });
      });
  bencher.body = [&](Bencher&)
  {
    for (auto [key, ask, responder] : asks.read())
      responder.respond(ToldResponse{ask.id});
    for (auto [key, tick] : ticks.readLatest())
      sum += tick.id;
    for (auto [key, ask, responder] : workAsks.read())
      pending.push_back(std::move(responder));
  };
  // A worker that responds to the pending requests when asked.
  std::atomic<int> phase{0};
  std::thread worker(
      [&]
      {
        for (;;)
        {
          const int now = phase.load();
          if (now == 2)
            return;
          if (now != 1)
            continue;
          for (const Responder<WorkAskRequest>& responder : pending)
            responder.respond(WorkToldResponse{responder.key().getId()});
          phase.store(0);
        }
      });
  std::vector<EventStats> stats;
  const auto frame = [&]
  {
    events.update();
    for (int i = 0; i < 256; ++i)
      events.send(TickEvent{static_cast<uint64_t>(i), {}});
    for (int i = 0; i < 16; ++i)
      broadcast.send(TickEvent{static_cast<uint64_t>(i), {}});
    for (int i = 0; i < 64; ++i)
    {
      const Entity& key = keys[static_cast<std::size_t>(i)];
      asker.request(key, AskRequest{static_cast<uint64_t>(i)});
      workAsker.request(key, WorkAskRequest{static_cast<uint64_t>(i)});
      events.send(HandledEvent{static_cast<uint64_t>(i), {}});
      events.send(FireDamageEvent{{static_cast<uint64_t>(i), 1.0f}, 2.0f, {}});
      events.send(key, KeyedTickEvent{static_cast<uint64_t>(i), {}});
    }
    registry.updateSystem(bencher, 0.0);
    phase.store(1);
    while (phase.load() != 0)
    {
    }
    pending.clear();
    for (const TickEvent& tick : first.read())
      sum += tick.id;
    for (const TickEvent& tick : second.read())
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
  const std::size_t after = metrics.allocationCount.load();
  phase.store(2);
  worker.join();
  g_sink = g_sink + sum;
  return static_cast<long long>(after - before);
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
    gate("read, 1 consumer <= 0.5 ns, or a vector's loop + 10%",
         one,
         std::max(0.5, 1.1 * vectorOne));
    gate("read, 4 consumers <= 0.5 ns, or a vector's loop + 10%",
         four,
         std::max(0.5, 1.1 * vectorFour));
  }

  TEST("a frame boundary should cost little, whatever is kept")
  {
    const double idle = benchIdleUpdate();
    const auto [few, many] = benchKeptUpdate();
    print("update-32-idle", 32, idle);
    print("update-consumed", 10, few);
    print("update-consumed", kEvents, many);
    gate("update, 32 idle queues <= 100 ns", idle, 100.0);
    // Clearing a consumed buffer costs the same whatever it held; the margin
    // is the cache misses on the queue's consumers after 10,000 sends to each
    // of 256 queues.
    gate("update, 10,000 consumed <= 10 consumed + 5% (or + 5 ns)",
         many,
         std::max(1.05 * few, few + 5.0));
    // Each event is moved at most about once as the slow consumer's are
    // compacted.
    const double slow = benchSlowReader();
    print("update-slow-consumer", kEvents, slow);
    gate("update with a slow consumer <= 0.5 ns per event sent", slow, 0.5);
  }

  TEST("broadcasts should cost little, alone and contended")
  {
    const auto [one, oneDrain] = benchBroadcast(1);
    const auto [four, fourDrain] = benchBroadcast(4);
    print("broadcast-1", kEvents, one);
    print("broadcast-4", kEvents, four);
    print("drain", kEvents, oneDrain);
    print("drain", 4 * kEvents, fourDrain);
    gate("EventBroadcast::send, 1 thread <= 50 ns", one, 50.0);
    gate("EventBroadcast::send, 4 contending <= 250 ns", four, 250.0);
    gate("drain <= 3 ns per event", std::max(oneDrain, fourDrain), 3.0);
  }

  TEST("keyed sends should cost little, superseding included")
  {
    const double many = benchKeyedSend(false);
    const double one = benchKeyedSend(true);
    print("keyed-send", kEvents, many);
    print("keyed-send-supersede", kEvents, one);
    // About 7 ns measured; the design's 8 with headroom for a loaded
    // machine.
    gate("keyed send <= 9 ns", many, 9.0);
    gate("keyed send superseding the key's last <= 9 ns", one, 9.0);
    const double request = benchRequest();
    print("requester-request", kEvents, request);
    // Its ticket, found through the shorter of the requester's and the
    // entity's lists (the heads, then the ticket in its block: dependent
    // loads over a keyed send) and the fourth lane: about 14.5 ns, with
    // headroom.
    gate("a requester's request <= 18 ns", request, 18.0);
  }

  TEST("handlers, latest reads, responders and responses should cost "
       "little an event")
  {
    const double loop = benchReadLoop();
    const double handler = benchHandler(false);
    const double batch = benchHandler(true);
    const double readLatest = benchLatest();
    const double responders = benchResponders();
    const double responses = benchResponses();
    const double worker = benchWorkerResponse();
    print("read-loop", kEvents, loop);
    print("on-event", kEvents, handler);
    print("on-events", kEvents, batch);
    print("read-latest", kEvents, readLatest);
    print("read-responders", kEvents, responders);
    print("on-response", kEvents, responses);
    print("worker-response", kEvents, worker);
    // A handler is a call through std::function per event; the batch one
    // a single call for the span.
    gate("onEvent <= a read loop + 1.5 ns an event", handler, loop + 1.5);
    gate("onEvents <= a read loop + 0.5 ns an event", batch, loop + 0.5);
    gate("readLatest <= 4 ns an event", readLatest, 4.0);
    // A row's responder borrows the request type's channel (no reference
    // count); each response is four appends, a ticket check, closing the
    // ticket and its requester's route: about 19 ns, with headroom.
    gate(
        "read with responders, responding <= 24 ns an event", responders, 24.0);
    gate("onResponse <= 4 ns a response", responses, 4.0);
    const double byEntity = benchRespondByEntity();
    print("respond-by-entity", kEvents, byEntity);
    // The key's stale check and its open tickets (one here), then a
    // response as a Responder's on this thread: about 17 ns, with headroom.
    gate("respond by entity <= 21 ns a response", byEntity, 21.0);
    // Two uncontended locks (the channel's slot list, the inbox) and an
    // append.
    gate("a worker's response <= 60 ns", worker, 60.0);
  }

  TEST("requests should hold memory per request and cost the flush little")
  {
    const double perRequest = benchRequesterMemory();
    const double none = benchRequesterFlush(false);
    const double asked = benchRequesterFlush(true);
    print("request-bytes", 20000, perRequest);
    print("flush-no-requests", kEvents, none);
    print("flush-with-requests", kEvents, asked);
    // A ticket (32 bytes) per request, plus its share of the blocks and of
    // the entity pages.
    gate("bytes per outstanding request <= 64", perRequest, 64.0);
    // Each destroyed entity's tickets: a page lookup and a short list.
    gate("flush with 1,000 requesters <= 1.3 x with none + 20 ns",
         asked,
         1.3 * none + 20.0);
  }

  TEST("many requesters should cost each the same as a few")
  {
    const double fanFew = benchFanIn(10);
    const double fanMany = benchFanIn(10000);
    const double goingFew = benchRequestersGoing(1000);
    const double goingMany = benchRequestersGoing(10000);
    const double manyFew = benchManyRequesters(100);
    const double manyMany = benchManyRequesters(4000);
    print("fan-in-10", 10, fanFew);
    print("fan-in-10000", 10000, fanMany);
    print("requesters-going-1000", 1000, goingFew);
    print("requesters-going-10000", 10000, goingMany);
    print("requesters-100", 100, manyFew);
    print("requesters-4000", 4000, manyMany);
    // A requester's ticket is found through its own one-ticket list.
    gate("request/waiting/cancel, 10,000 on one entity <= 2 x 10 + 5 ns",
         fanMany,
         2.0 * fanFew + 5.0);
    gate("a requester going, of 10,000 <= 2 x of 1,000 + 20 ns",
         goingMany,
         2.0 * goingFew + 20.0);
    // Each requester goes through its own routed responses only.
    gate("handling a response, 4,000 requesters <= 2 x 100 + 20 ns",
         manyMany,
         2.0 * manyFew + 20.0);
  }

  TEST("news with a consumed event base should cost about two sends")
  {
    const double plain = benchFamilySend(true);
    const double family = benchFamilySend(false);
    print("send-unused-base", kEvents, plain);
    print("send-with-base", kEvents, family);
    // Two appends (the event, then its copy as the base) and the base's
    // consumer check.
    gate("send with one consumed base <= 2.5 x one with none",
         family,
         2.5 * plain);
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
