#include "../../testHarness.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/events/eventQueue.h"
#include "engine/core/events/eventRows.h"
#include "engine/core/events/events.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/allocationMetrics.h"

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A system's event handles (EventVerbs): consumers made in create(), their
// handlers run before update(), one span per update, events kept until every
// consumer has passed them, and the cap (docs/core/events).

using namespace sfs;

namespace
{

// A response handler that does nothing, for requesters that only ask.
constexpr auto IGNORED = [](const Entity&, const auto&) {};

struct [[sfs::event_news]] PingEvent
{
  int value = 0;
};

struct [[sfs::event_news]] JobEvent
{
  int value = 0;
};

struct [[sfs::event_entity_news]] OrderEvent
{
  int value = 0;
};

// One type per test that checks its own once-per-type log lines or counts.
struct [[sfs::event_news]] DisabledEvent
{
  int value = 0;
};
struct [[sfs::event_news]] HookedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] HookedLaterEvent
{
  int value = 0;
};
struct [[sfs::event_news]] RenderedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] CappedEvent
{
  int value = 0;
};
struct [[sfs::event_entity_news]] LatestCappedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] LookupEvent
{
  int value = 0;
};
struct [[sfs::event_entity_news]] LookupLatestEvent
{
  int value = 0;
};
struct [[sfs::event_news]] MadeLateEvent
{
  int value = 0;
};
struct [[sfs::event_news]] OwnerlessEvent
{
  int value = 0;
};
struct [[sfs::event_entity_news]] SentWhileEvent
{
  int value = 0;
};
struct [[sfs::event_news]] ChainEvent
{
  int depth = 0;
};
struct [[sfs::event_entity_news]] KeyedChainEvent
{
  int value = 0;
};
struct [[sfs::event_news]] LateReadEvent
{
  int value = 0;
};
struct [[sfs::event_news]] OutlivedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] StopEvent
{
  int value = 0;
};
struct [[sfs::event_news]] SkippedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] CountedEvent
{
  int value = 0;
};
struct [[sfs::event_entity_news]] KeyedSlowEvent
{
  int value = 0;
};
struct [[sfs::event_news]] HandledEvent
{
  int value = 0;
};
struct [[sfs::event_entity_news]] HandledKeyedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] SelfRemovedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] LateHandlerEvent
{
  int value = 0;
};
struct [[sfs::event_news]] ReusedSlotEvent
{
  int value = 0;
};
struct [[sfs::event_news]] AlarmEvent
{
  int value = 0;
};
struct [[sfs::event_entity_news]] AlertEvent
{
  int value = 0;
};
struct [[sfs::event_news]] LootEvent
{
  int value = 0;
};
struct [[sfs::event_response]] AskedResponse
{
  int value = 0;
};
struct [[sfs::event_request(AskedResponse)]] AskedRequest
{
  int value = 0;
};

// Collects what is logged while it lives.
struct LogCapture
{
  std::vector<std::pair<Logger::Level, std::string>> lines;
  Logger::SinkId sink;

  LogCapture()
  {
    Logger::flush();
    sink =
        Logger::addSink([this](Logger::Level level, const std::string& message)
                        { lines.emplace_back(level, message); });
  }
  ~LogCapture()
  {
    Logger::removeSink(sink);
    Logger::flush();
  }
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;

  int count(Logger::Level level, std::string_view part) const
  {
    int found = 0;

    for (const auto& [at, line] : lines)
      if (at == level && line.find(part) != std::string::npos)
        ++found;

    return found;
  }
};

// A system whose create(), update(), lateUpdate(), render() and hook run
// what a test gives them, with its event calls made public for the test.
// A test's handles are declared after its registry, so they go before the
// systems that own them.
class Probe : public System
{
public:
  /** @param setUp what it does in create(): make its handles */
  explicit Probe(std::function<void(Probe&)> setUp = {})
      : m_setUp(std::move(setUp))
  {
  }

  using EventVerbs::consumer;
  using EventVerbs::requester;
  using EventVerbs::send;

  std::function<void(Probe&)> body;
  std::function<void(Probe&)> late;
  std::function<void(Probe&)> hook;
  std::function<void(Probe&)> drawn;

  /** Runs render() as a render system would. */
  void renderNow() { render(); }
  /** Runs update() directly, as a host shouldn't. */
  void updateNow() { update(0.0); }

protected:
  void create() override
  {
    registerComponent<int>();

    if (m_setUp)
      m_setUp(*this);
  }
  void update(double) override
  {
    if (body)
      body(*this);
  }
  void lateUpdate(double) override
  {
    if (late)
      late(*this);
  }
  void render() override
  {
    if (drawn)
      drawn(*this);
  }
  void onEntityAdded(const Entity&) override
  {
    if (hook)
      hook(*this);
  }

private:
  std::function<void(Probe&)> m_setUp;
};

// A probe of another type, so a registry can hold two.
class OtherProbe : public Probe
{
public:
  using Probe::Probe;
};

// A system that keeps its consumer of E as a member.
template <typename TEvent>
class Holder : public System
{
public:
  EventConsumer<TEvent> events;

protected:
  void create() override { events = consumer<TEvent>(); }
};

// A guard post whose handlers are its member functions: alarms on their
// own, alerts about one guard, and a const member that only looks.
class GuardPost : public System
{
public:
  std::vector<int> alarms;
  std::vector<std::size_t> alarmBatches;
  std::vector<std::pair<Entity, int>> alerts;
  std::vector<int> newest;
  std::vector<std::size_t> newestBatches;
  mutable int looked = 0;

protected:
  void create() override
  {
    m_alarms = consumer<AlarmEvent>().onEvent(&GuardPost::onAlarm);
    m_alarmBatches = consumer<AlarmEvent>().onEvents(&GuardPost::onAlarms);
    m_looks = consumer<AlarmEvent>().onEvent(&GuardPost::look);
    m_alerts = consumer<AlertEvent>()
                   .onEvent(&GuardPost::onAlert)
                   .onLatest(&GuardPost::onNewestAlert)
                   .onLatestEvents(&GuardPost::onNewestAlerts);
  }
  void update(double) override {}

private:
  void look(const AlarmEvent&) const { ++looked; }
  void onAlarm(const AlarmEvent& alarm) { alarms.push_back(alarm.value); }
  void onAlarms(EventRange<AlarmEvent> batch)
  {
    alarmBatches.push_back(batch.size());
  }
  void onAlert(const Entity& guard, const AlertEvent& alert)
  {
    alerts.emplace_back(guard, alert.value);
  }
  void onNewestAlert(const Entity&, const AlertEvent& alert)
  {
    newest.push_back(alert.value);
  }
  void onNewestAlerts(LatestEventRange<AlertEvent> batch)
  {
    newestBatches.push_back(batch.size());
  }

  EventConsumer<AlarmEvent> m_alarms;
  EventConsumer<AlarmEvent> m_alarmBatches;
  EventConsumer<AlarmEvent> m_looks;
  EventConsumer<AlertEvent> m_alerts;
};

// A looter whose loot handler is public, so another system can name it.
class Looter : public System
{
public:
  int looted = 0;

  void onLoot(const LootEvent&) { ++looted; }

protected:
  void update(double) override {}
};

// Gives its consumer a member function of Looter, which it isn't.
class Thief : public System
{
public:
  EventConsumer<LootEvent> loot;

protected:
  void create() override
  {
    loot = consumer<LootEvent>().onEvent(&Looter::onLoot);
  }
  void update(double) override {}
};

// A set-up that makes `into` a consumer of TEvent.
template <typename TEvent>
std::function<void(Probe&)> consuming(EventConsumer<TEvent>& into)
{
  return [&into](Probe& probe) { into = probe.consumer<TEvent>(); };
}

// One frame boundary, as Scene::update() makes it.
void frame(Registry& registry)
{
  registry.flushEntities();
  registry.events().update();
}

// Runs a probe's update once with `body`.
void run(Registry& registry,
         Probe& probe,
         std::function<void(Probe&)> body = {})
{
  probe.body = std::move(body);
  registry.updateSystem(probe, 0.0);
  probe.body = {};
}

// The values a probe's update reads from `consumer`.
template <typename TEvent>
std::vector<int>
values(Registry& registry, Probe& probe, EventConsumer<TEvent>& consumer)
{
  std::vector<int> got;
  run(registry,
      probe,
      [&](Probe&)
      {
        for (const TEvent& event : consumer.read())
          got.push_back(event.value);
      });
  return got;
}

// The values a probe's update reads with readLatest() from `consumer`.
template <typename TEvent>
std::vector<int>
latest(Registry& registry, Probe& probe, EventConsumer<TEvent>& consumer)
{
  std::vector<int> got;
  run(registry,
      probe,
      [&](Probe&)
      {
        for (auto [entity, event] : consumer.readLatest())
          got.push_back(event.value);
      });
  return got;
}

// The stats of the type whose name ends with `name`.
EventStats statsOf(Registry& registry, std::string_view name)
{
  std::vector<EventStats> stats;
  registry.events().stats(stats);

  for (const EventStats& type : stats)
    if (type.type.ends_with(name))
      return type;

  return {};
}

// The kept count of the type whose name ends with `name`.
std::size_t kept(Registry& registry, std::string_view name)
{
  return statsOf(registry, name).kept;
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

} // namespace

int main()
{
  TEST("read should give the same span within an update, a new one each "
       "update and lateUpdate")
  {
    Registry registry;
    EventConsumer<PingEvent> pings;
    Probe& probe = registry.addSystem<Probe>(consuming(pings));
    registry.events().send(PingEvent{1});
    std::vector<int> first;
    std::vector<int> second;
    run(registry,
        probe,
        [&](Probe& p)
        {
          for (const PingEvent& ping : pings.read())
            first.push_back(ping.value);

          p.send(PingEvent{2});

          for (const PingEvent& ping : pings.read())
            second.push_back(ping.value);
        });
    CHECK((first == std::vector<int>{1}));
    CHECK((second == std::vector<int>{1}));
    std::vector<int> late;
    probe.late = [&](Probe&)
    {
      for (const PingEvent& ping : pings.read())
        late.push_back(ping.value);
    };
    registry.lateUpdateSystem(probe, 0.0);
    probe.late = {};
    CHECK((late == std::vector<int>{2}));
    CHECK(values(registry, probe, pings).empty());
  }

  TEST("a system added after sends should see the events still kept")
  {
    Registry registry;
    registry.events().send(PingEvent{1});
    frame(registry);
    registry.events().send(PingEvent{2});
    EventConsumer<PingEvent> pings;
    Probe& probe = registry.addSystem<Probe>(consuming(pings));
    CHECK((values(registry, probe, pings) == std::vector<int>{1, 2}));
  }

  TEST("a consumer or requester made outside create() should be logged and "
       "receive nothing")
  {
    Registry registry;
    Probe& probe = registry.addSystem<Probe>();
    LogCapture log;
    EventConsumer<MadeLateEvent> late;
    EventRequester<AskedRequest> asker;
    run(registry,
        probe,
        [&](Probe& p)
        {
          late = p.consumer<MadeLateEvent>();
          asker = p.requester<AskedRequest>().onResponse(IGNORED);
        });
    CHECK(!late.valid());
    CHECK(!asker.valid());
    registry.events().send(MadeLateEvent{1});
    CHECK(values(registry, probe, late).empty());
    CHECK(log.count(Logger::Level::ERROR, "Probe made a consumer of") == 1);
    CHECK(log.count(Logger::Level::ERROR, "outside create()") == 1);
  }

  TEST("handlers on a consumer with no owner should be logged and never run")
  {
    Registry registry;
    LogCapture log;
    int ran = 0;
    EventConsumer<OwnerlessEvent> loose =
        registry.events().consumer<OwnerlessEvent>().onEvent(
            [&](const OwnerlessEvent&) { ++ran; });
    registry.events().send(OwnerlessEvent{1});
    frame(registry);
    CHECK(ran == 0);
    CHECK(loose.read().size() == 1u);
    CHECK(log.count(Logger::Level::ERROR, "with no owner") == 1);
  }

  TEST("onEvent should run for each new event before update(), and not at "
       "all when nothing is new")
  {
    Registry registry;
    EventConsumer<HandledEvent> handled;
    std::vector<int> seen;
    std::vector<std::string> order;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          handled = p.consumer<HandledEvent>().onEvent(
              [&](const HandledEvent& event)
              {
                seen.push_back(event.value);
                order.emplace_back("handler");
              });
        });
    registry.events().send(HandledEvent{1});
    registry.events().send(HandledEvent{2});
    std::size_t readInUpdate = 0;
    run(registry,
        probe,
        [&](Probe&)
        {
          order.emplace_back("update");
          // The same span the handler got.
          readInUpdate = handled.read().size();
        });
    CHECK((seen == std::vector<int>{1, 2}));
    CHECK((order == std::vector<std::string>{"handler", "handler", "update"}));
    CHECK(readInUpdate == 2u);
    frame(registry);
    run(registry, probe);
    CHECK(seen.size() == 2u);
    registry.events().send(HandledEvent{3});
    run(registry, probe);
    CHECK((seen == std::vector<int>{1, 2, 3}));
  }

  TEST("member functions of the owner should handle events as lambdas do, "
       "with the entity and without, const or not")
  {
    Registry registry;
    GuardPost& post = registry.addSystem<GuardPost>();
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.events().send(AlarmEvent{1});
    registry.events().send(AlarmEvent{2});
    registry.events().send(a, AlertEvent{10});
    registry.events().send(b, AlertEvent{20});
    registry.events().send(a, AlertEvent{30});
    registry.updateSystem(post, 0.0);
    CHECK((post.alarms == std::vector<int>{1, 2}));
    CHECK((post.alarmBatches == std::vector<std::size_t>{2}));
    CHECK(post.looked == 2);
    CHECK(post.alerts.size() == 3u);
    CHECK((post.alerts[0] == std::pair<Entity, int>{a, 10}));
    CHECK((post.alerts[1] == std::pair<Entity, int>{b, 20}));
    CHECK((post.alerts[2] == std::pair<Entity, int>{a, 30}));
    CHECK((post.newest == std::vector<int>{20, 30}));
    CHECK((post.newestBatches == std::vector<std::size_t>{2}));
    frame(registry);
    registry.updateSystem(post, 0.0);
    CHECK(post.alarms.size() == 2u);
    CHECK(post.alarmBatches.size() == 1u);
  }

  TEST("a member function of a class that isn't the owner should be logged "
       "and never run")
  {
    LogCapture log;
    Registry registry;
    Looter& looter = registry.addSystem<Looter>();
    Thief& thief = registry.addSystem<Thief>();
    CHECK(log.count(Logger::Level::ERROR, "is a member function of") == 1);
    CHECK(thief.loot.valid());
    registry.events().send(LootEvent{1});
    registry.updateSystem(thief, 0.0);
    registry.updateSystem(looter, 0.0);
    CHECK(looter.looted == 0);
  }

  TEST("onEvents should get each update's events at once, and onLatest and "
       "onLatestEvents each key's newest")
  {
    Registry registry;
    EventConsumer<HandledEvent> batch;
    EventConsumer<HandledKeyedEvent> keyed;
    std::vector<std::size_t> batches;
    std::vector<int> newest;
    std::vector<std::size_t> latestBatches;
    std::vector<int> all;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          batch = p.consumer<HandledEvent>().onEvents(
              [&](EventRange<HandledEvent> events)
              { batches.push_back(events.size()); });
          keyed = p.consumer<HandledKeyedEvent>();
          keyed
              .onLatest([&](const Entity&, const HandledKeyedEvent& event)
                        { newest.push_back(event.value); })
              .onLatestEvents([&](LatestEventRange<HandledKeyedEvent> events)
                              { latestBatches.push_back(events.size()); })
              .onEvent([&](const Entity&, const HandledKeyedEvent& event)
                       { all.push_back(event.value); });
        });
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();

    for (int i = 0; i < 3; ++i)
      registry.events().send(HandledEvent{i});

    registry.events().send(a, HandledKeyedEvent{1});
    registry.events().send(b, HandledKeyedEvent{2});
    registry.events().send(a, HandledKeyedEvent{3});
    run(registry, probe);
    CHECK((batches == std::vector<std::size_t>{3}));
    CHECK((newest == std::vector<int>{2, 3}));
    CHECK((latestBatches == std::vector<std::size_t>{2}));
    CHECK((all == std::vector<int>{1, 2, 3}));
    run(registry, probe);
    CHECK(batches.size() == 1u);
    CHECK(latestBatches.size() == 1u);
  }

  TEST("a moved consumer should keep its place and handlers, and a destroyed "
       "one stop holding events and running them")
  {
    Registry registry;
    EventConsumer<HandledEvent> first;
    int ran = 0;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          first = p.consumer<HandledEvent>().onEvent([&](const HandledEvent&)
                                                     { ++ran; });
        });
    registry.events().send(HandledEvent{1});
    EventConsumer<HandledEvent> moved = std::move(first);
    CHECK(!first.valid());
    CHECK(moved.valid());
    CHECK(moved.unread() == 1u);
    run(registry, probe);
    CHECK(ran == 1);
    moved = EventConsumer<HandledEvent>();
    registry.events().send(HandledEvent{2});
    run(registry, probe);
    CHECK(ran == 1);
    frame(registry);
    frame(registry);
    CHECK(statsOf(registry, "::HandledEvent").consumers == 0u);
    CHECK(kept(registry, "::HandledEvent") == 0u);
  }

  TEST("a handler that destroys its consumer should be safe, and one given "
       "while handlers run refused")
  {
    Registry registry;
    EventConsumer<SelfRemovedEvent> removed;
    EventConsumer<LateHandlerEvent> late;
    int ran = 0;
    int lateRan = 0;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          removed = p.consumer<SelfRemovedEvent>().onEvent(
              [&](const SelfRemovedEvent&)
              {
                ++ran;
                removed = EventConsumer<SelfRemovedEvent>();
              });
          late = p.consumer<LateHandlerEvent>().onEvent(
              [&](const LateHandlerEvent&)
              { late.onEvent([&](const LateHandlerEvent&) { ++lateRan; }); });
        });
    LogCapture log;
    registry.events().send(SelfRemovedEvent{1});
    registry.events().send(SelfRemovedEvent{2});
    registry.events().send(LateHandlerEvent{1});
    // The span being handled is finished; nothing after.
    run(registry, probe);
    CHECK(ran == 2);
    CHECK(!removed.valid());
    registry.events().send(SelfRemovedEvent{3});
    registry.events().send(LateHandlerEvent{2});
    run(registry, probe);
    CHECK(ran == 2);
    CHECK(lateRan == 0);
    CHECK(log.count(Logger::Level::ERROR, "while its owner's handlers ran") ==
          1);
  }

  TEST("consumers going should stop their handlers, few or many, and a slot "
       "given to another owner's consumer run only its new handler")
  {
    Registry registry;
    std::vector<EventConsumer<ReusedSlotEvent>> held;
    int stayed = 0;
    int went = 0;
    int other = 0;
    Probe& first = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          held.push_back(p.consumer<ReusedSlotEvent>().onEvent(
              [&](const ReusedSlotEvent&) { ++stayed; }));

          for (int i = 0; i < 4; ++i)
            held.push_back(p.consumer<ReusedSlotEvent>().onEvent(
                [&](const ReusedSlotEvent&) { ++went; }));
        });
    // One of five going leaves its entry to be cleared later; the slot it
    // freed goes to the other probe's consumer.
    held.erase(held.begin() + 1);
    EventConsumer<ReusedSlotEvent> reused;
    OtherProbe& second = registry.addSystem<OtherProbe>(
        [&](Probe& p)
        {
          reused = p.consumer<ReusedSlotEvent>().onEvent(
              [&](const ReusedSlotEvent&) { ++other; });
        });
    registry.events().send(ReusedSlotEvent{1});
    run(registry, first);
    run(registry, second);
    CHECK(stayed == 1);
    CHECK(went == 3);
    CHECK(other == 1);
    held.erase(held.begin() + 1, held.end());
    registry.events().send(ReusedSlotEvent{2});
    run(registry, first);
    run(registry, second);
    CHECK(stayed == 2);
    CHECK(went == 3);
    CHECK(other == 2);
    CHECK(statsOf(registry, "::ReusedSlotEvent").consumers == 2u);
  }

  TEST("a read in lateUpdate() should start its own span, which the next "
       "update's handlers then don't get")
  {
    Registry registry;
    EventConsumer<LateReadEvent> late;
    int handled = 0;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          late = p.consumer<LateReadEvent>().onEvent([&](const LateReadEvent&)
                                                     { ++handled; });
        });
    std::size_t readLate = 0;
    probe.late = [&](Probe&) { readLate += late.read().size(); };
    registry.events().send(LateReadEvent{1});
    registry.lateUpdateSystem(probe, 0.0);
    run(registry, probe);
    CHECK(readLate == 1u);
    CHECK(handled == 0);
    probe.late = {};
  }

  TEST("a consumer that outlives its owning system should go safely")
  {
    EventConsumer<OutlivedEvent> outlived;
    Registry registry;
    registry.addSystem<Probe>(
        [&](Probe& p)
        {
          outlived =
              p.consumer<OutlivedEvent>().onEvent([](const OutlivedEvent&) {});
        });
    registry.removeSystem<Probe>();
    registry.events().send(OutlivedEvent{1});
    CHECK(outlived.valid());
    CHECK(outlived.read().size() == 1u);
    outlived = EventConsumer<OutlivedEvent>();
    CHECK(!outlived.valid());
  }

  TEST("a handler that disables its system should stop that update")
  {
    Registry registry;
    EventConsumer<StopEvent> stops;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          stops = p.consumer<StopEvent>().onEvent([&p](const StopEvent&)
                                                  { p.setEnabled(false); });
        });
    int updates = 0;
    registry.events().send(StopEvent{1});
    run(registry, probe, [&](Probe&) { ++updates; });
    CHECK(updates == 0);
  }

  TEST("skip() should pass this update's span too")
  {
    Registry registry;
    EventConsumer<SkippedEvent> skipped;
    Probe& probe = registry.addSystem<Probe>(consuming(skipped));
    registry.events().send(SkippedEvent{1});
    std::size_t before = 0;
    std::size_t after = 1;
    run(registry,
        probe,
        [&](Probe&)
        {
          before = skipped.read().size();
          skipped.skip();
          after = skipped.read().size();
        });
    CHECK(before == 1u);
    CHECK(after == 0u);
  }

  TEST("a disabled system should get every event once it's enabled again")
  {
    Registry registry;
    EventConsumer<DisabledEvent> disabled;
    Probe& probe = registry.addSystem<Probe>(consuming(disabled));
    probe.setEnabled(false);

    for (int i = 0; i < 5; ++i)
    {
      registry.events().send(DisabledEvent{i});
      frame(registry);
    }

    probe.setEnabled(true);
    CHECK(
        (values(registry, probe, disabled) == std::vector<int>{0, 1, 2, 3, 4}));
    frame(registry);
    CHECK(kept(registry, "::DisabledEvent") == 0u);
  }

  TEST("a system that reads late should miss nothing, and the queue keep "
       "only what it hasn't passed")
  {
    Registry registry;
    EventConsumer<JobEvent> fastJobs;
    EventConsumer<JobEvent> slowJobs;
    Probe& fast = registry.addSystem<Probe>(consuming(fastJobs));
    Probe& slow = registry.addSystem<Probe>(consuming(slowJobs));

    for (int i = 0; i < 10; ++i)
    {
      registry.events().send(JobEvent{i});
      values(registry, fast, fastJobs);
      frame(registry);
    }

    CHECK(kept(registry, "::JobEvent") == 10u);
    CHECK(values(registry, slow, slowJobs).size() == 10u);
    frame(registry);
    CHECK(kept(registry, "::JobEvent") == 0u);
  }

  TEST("past the cap the oldest events should be dropped, naming the "
       "consumer that holds them, and counted as its misses")
  {
    Registry registry;
    registry.events().setMaxKept(4);
    EventConsumer<CappedEvent> capped;
    Probe& stuck = registry.addSystem<Probe>(consuming(capped));
    stuck.setEnabled(false);
    LogCapture log;

    for (int i = 0; i < 10; ++i)
      registry.events().send(CappedEvent{i});

    frame(registry);
    stuck.setEnabled(true);
    CHECK((values(registry, stuck, capped) == std::vector<int>{6, 7, 8, 9}));
    CHECK(log.count(Logger::Level::ERROR, "Probe, which hasn't read them") ==
          1);
    CHECK(log.count(Logger::Level::WARN, "Probe missed 6") == 1);
    CHECK(log.count(Logger::Level::WARN, "while it was disabled") == 1);
    CHECK(statsOf(registry, "::CappedEvent").missed == 6u);
  }

  TEST("a latest consumer should count as missed the current events the cap "
       "drops")
  {
    Registry registry;
    registry.events().setMaxKept(2);
    EventConsumer<LatestCappedEvent> reader;
    EventConsumer<LatestCappedEvent> held;
    Probe& stuck = registry.addSystem<Probe>(consuming(reader));
    latest(registry, stuck, reader);
    // A consumer that holds every event, so the cap is what drops them.
    Probe& holder = registry.addSystem<Probe>(consuming(held));
    holder.setEnabled(false);

    for (int i = 0; i < 5; ++i)
      registry.events().send(registry.createEntity(), LatestCappedEvent{i});

    frame(registry);
    // The latest consumer misses the 3 current ones now; the holder counts
    // its own when it next reads.
    CHECK(statsOf(registry, "::LatestCappedEvent").missed == 3u);
  }

  TEST("a removed system should stop holding its types' events")
  {
    Registry registry;
    registry.addSystem<Holder<PingEvent>>();
    registry.events().send(PingEvent{1});
    frame(registry);
    frame(registry);
    EventStats stats = statsOf(registry, "::PingEvent");
    CHECK((stats.kept == 1u && stats.consumers == 1u));
    registry.removeSystem<Holder<PingEvent>>();
    frame(registry);
    frame(registry);
    stats = statsOf(registry, "::PingEvent");
    CHECK((stats.kept == 0u && stats.consumers == 0u));
  }

  TEST("hasUnconsumedEvents should say whether a consumer has events to read")
  {
    Registry registry;
    EventConsumer<OrderEvent> orders;
    Probe& probe = registry.addSystem<Probe>(consuming(orders));
    CHECK(!probe.hasUnconsumedEvents());
    registry.events().send(registry.createEntity(), OrderEvent{1});
    CHECK(probe.hasUnconsumedEvents());
    latest(registry, probe, orders);
    CHECK(!probe.hasUnconsumedEvents());
  }

  TEST("reading from a hook should be logged (builds without NDEBUG)")
  {
    Registry registry;
    EventConsumer<HookedEvent> hooked;
    Probe& probe = registry.addSystem<Probe>(consuming(hooked));
    LogCapture log;
    probe.hook = [&](Probe&) { hooked.read(); };
    registry.createEntity().addComponent<int>(1);
    registry.flushEntities();
#ifndef NDEBUG
    CHECK(log.count(Logger::Level::ERROR, "outside its update") == 1);
#else
    CHECK(log.count(Logger::Level::ERROR, "outside its update") == 0);
#endif
  }

  TEST("a hook, render() or direct update() after an update should get only "
       "what's new, never the update's events again")
  {
    Registry registry;
    EventConsumer<HookedLaterEvent> hooked;
    EventConsumer<RenderedEvent> rendered;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          hooked = p.consumer<HookedLaterEvent>();
          rendered = p.consumer<RenderedEvent>();
        });
    const auto readHooked = [&](std::vector<int>& into)
    {
      for (const HookedLaterEvent& event : hooked.read())
        into.push_back(event.value);
    };
    registry.events().send(HookedLaterEvent{1});
    std::vector<int> inUpdate;
    run(registry, probe, [&](Probe&) { readHooked(inUpdate); });
    CHECK((inUpdate == std::vector<int>{1}));
    // Same frame: an entity joins and the hook reads.
    std::vector<int> inHook;
    probe.hook = [&](Probe&) { readHooked(inHook); };
    registry.createEntity().addComponent<int>(1);
    registry.flushEntities();
    CHECK(inHook.empty());
    // Next frame, before the update: the hook reads only what's sent since.
    registry.events().update();
    registry.events().send(HookedLaterEvent{2});
    inHook.clear();
    registry.createEntity().addComponent<int>(2);
    registry.flushEntities();
    CHECK((inHook == std::vector<int>{2}));
    probe.hook = {};
    // render() and a direct update() likewise.
    registry.events().send(RenderedEvent{3});
    std::vector<int> seen;
    probe.drawn = [&](Probe&)
    {
      for (const RenderedEvent& event : rendered.read())
        seen.push_back(event.value);
    };
    run(registry, probe, probe.drawn);
    probe.renderNow();
    probe.body = probe.drawn;
    probe.updateNow();
    probe.body = {};
    CHECK((seen == std::vector<int>{3}));
  }

  TEST("superseded keyed events shouldn't be held by latest consumers")
  {
    Registry registry;
    EventConsumer<OrderEvent> orders;
    Probe& probe = registry.addSystem<Probe>(consuming(orders));
    latest(registry, probe, orders);
    probe.setEnabled(false);
    const Entity a = registry.createEntity();

    for (int i = 0; i < 5; ++i)
      registry.events().send(a, OrderEvent{i});

    frame(registry);
    CHECK(kept(registry, "::OrderEvent") == 1u);
    probe.setEnabled(true);
    CHECK((latest(registry, probe, orders) == std::vector<int>{4}));
  }

  TEST("read of a keyed type should give every event; readLatest the newest")
  {
    Registry registry;
    EventConsumer<OrderEvent> all;
    EventConsumer<OrderEvent> newest;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          all = p.consumer<OrderEvent>();
          newest = p.consumer<OrderEvent>();
        });
    const Entity a = registry.createEntity();
    registry.events().send(a, OrderEvent{1});
    registry.events().send(a, OrderEvent{2});
    std::vector<int> every;
    std::vector<int> last;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, order] : all.read())
            every.push_back(order.value);

          for (auto [entity, order] : newest.readLatest())
            last.push_back(order.value);
        });
    CHECK((every == std::vector<int>{1, 2}));
    CHECK((last == std::vector<int>{2}));
  }

  TEST("compacting behind a slow consumer should keep keys beside their "
       "events")
  {
    Registry registry;
    EventConsumer<KeyedSlowEvent> slowEvents;
    EventConsumer<KeyedSlowEvent> offsetEvents;
    EventConsumer<KeyedSlowEvent> everyEvents;
    Probe& slow = registry.addSystem<Probe>(consuming(slowEvents));
    Probe& offset = registry.addSystem<Probe>(consuming(offsetEvents));
    Probe& every = registry.addSystem<Probe>(consuming(everyEvents));
    std::vector<Entity> keys;

    for (int i = 0; i < 4; ++i)
      keys.push_back(registry.createEntity());

    std::vector<int> read;
    std::vector<int> readOffset;
    std::vector<int> readEvery;
    bool keysMatch = true;
    const auto readInto = [&](Probe& reader,
                              EventConsumer<KeyedSlowEvent>& consumer,
                              std::vector<int>& into)
    {
      run(registry,
          reader,
          [&](Probe&)
          {
            for (auto [entity, event] : consumer.read())
            {
              into.push_back(event.value);
              keysMatch =
                  keysMatch &&
                  entity == keys[static_cast<std::size_t>(event.value % 4)];
            }
          });
    };
    int next = 0;

    for (int frameIndex = 0; frameIndex < 12; ++frameIndex)
    {
      for (int i = 0; i < 4; ++i, ++next)
        registry.events().send(
            keys[static_cast<std::size_t>(i)], KeyedSlowEvent{next});

      readInto(every, everyEvents, readEvery);

      // Two consumers read every third frame, a frame apart, so the head
      // stops between them and the buffer's front is erased behind it.
      if (frameIndex % 3 == 2)
        readInto(slow, slowEvents, read);

      if (frameIndex % 3 == 0)
        readInto(offset, offsetEvents, readOffset);

      frame(registry);
    }

    readInto(offset, offsetEvents, readOffset);
    std::vector<int> all;

    for (int i = 0; i < next; ++i)
      all.push_back(i);

    CHECK(readEvery == all);
    CHECK(read == all);
    CHECK(readOffset == all);
    CHECK(keysMatch);
  }

  TEST("sending a keyed type while going through its rows should keep each "
       "row's entity and event, and give the new ones next update")
  {
    Registry registry;
    EventConsumer<SentWhileEvent> sent;
    Probe& keyed = registry.addSystem<Probe>(consuming(sent));
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.events().send(a, SentWhileEvent{1});
    registry.events().send(b, SentWhileEvent{2});
    std::vector<std::pair<Entity, int>> seen;
    run(registry,
        keyed,
        [&](Probe& p)
        {
          for (auto [entity, event] : sent.readLatest())
          {
            for (int i = 0; i < 64; ++i)
              p.send(entity, SentWhileEvent{10 + i});

            // The row is read after the sends.
            seen.emplace_back(entity, event.value);
          }
        });
    CHECK((seen == std::vector<std::pair<Entity, int>>{{a, 1}, {b, 2}}));
    CHECK((latest(registry, keyed, sent) == std::vector<int>{73, 73}));
  }

  TEST("a handler sending its own type should handle every event, and the "
       "ones it sent at the next update")
  {
    Registry registry;
    EventConsumer<ChainEvent> chain;
    std::vector<int> seen;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          chain = p.consumer<ChainEvent>().onEvent(
              [&](const ChainEvent& event)
              {
                seen.push_back(event.depth);

                if (event.depth < 2)
                  for (int i = 0; i < 40; ++i)
                    p.send(ChainEvent{event.depth + 1});
              });
        });
    registry.events().send(ChainEvent{0});
    registry.events().send(ChainEvent{0});
    run(registry, probe);
    CHECK((seen == std::vector<int>{0, 0}));
    run(registry, probe);
    CHECK(seen.size() == 82u);
    run(registry, probe);
    CHECK(seen.size() == 82u + 80u * 40u);
  }

  TEST("a keyed handler sending its own type should keep its rows")
  {
    Registry registry;
    EventConsumer<KeyedChainEvent> chain;
    std::vector<std::pair<Entity, int>> seen;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          chain = p.consumer<KeyedChainEvent>().onEvent(
              [&](const Entity& entity, const KeyedChainEvent& event)
              {
                for (int i = 0; i < 64; ++i)
                  p.send(entity, KeyedChainEvent{event.value + 1});

                seen.emplace_back(entity, event.value);
              });
        });
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.events().send(a, KeyedChainEvent{0});
    registry.events().send(b, KeyedChainEvent{5});
    run(registry, probe);
    CHECK((seen == std::vector<std::pair<Entity, int>>{{a, 0}, {b, 5}}));
  }

  TEST("an event call on a system outside a registry should log and do "
       "nothing")
  {
    LogCapture log;
    struct Runner : Probe
    {
      void go() { update(0.0); }
    };
    Runner runner;
    bool valid = true;
    runner.body = [&](Probe& p)
    {
      p.send(PingEvent{1});
      valid = p.consumer<PingEvent>().valid();
    };
    runner.go();
    CHECK(!valid);
    CHECK(log.count(Logger::Level::ERROR, "used events before it had them") ==
          1);
  }

  TEST("sentLastFrame should go to 0 once a frame sends nothing")
  {
    Registry registry;
    EventConsumer<CountedEvent> counted;
    Probe& probe = registry.addSystem<Probe>(consuming(counted));

    for (int i = 0; i < 5; ++i)
      registry.events().send(CountedEvent{i});

    values(registry, probe, counted);
    frame(registry);
    CHECK(statsOf(registry, "::CountedEvent").sentLastFrame == 5u);
    frame(registry);
    frame(registry);
    CHECK(statsOf(registry, "::CountedEvent").sentLastFrame == 0u);
  }

  TEST("consumerStats should list each consumer, its owner and how it reads")
  {
    Registry registry;
    EventConsumer<OrderEvent> reads;
    EventConsumer<OrderEvent> latestReads;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          reads = p.consumer<OrderEvent>();
          latestReads = p.consumer<OrderEvent>();
        });
    EventConsumer<OrderEvent> loose = registry.events().consumer<OrderEvent>();
    registry.events().send(registry.createEntity(), OrderEvent{1});
    run(registry,
        probe,
        [&](Probe&)
        {
          reads.read();
          latestReads.readLatest();
        });
    registry.events().send(registry.createEntity(), OrderEvent{2});
    std::vector<EventConsumerStats> rows;
    registry.events().consumerStats(rows);
    CHECK(rows.size() == 3u);
    int read = 0;
    int readLatest = 0;
    int unread = 0;

    for (const EventConsumerStats& row : rows)
    {
      read += row.how == "read" && row.consumer.ends_with("Probe") &&
              row.unconsumed == 1u;
      readLatest += row.how == "readLatest" && row.unconsumed == 1u;
      unread += row.how.empty() && row.consumer == "a consumer" &&
                row.unconsumed == 2u;
    }

    CHECK((read == 1 && readLatest == 1 && unread == 1));
  }

  TEST("reads and handlers should allocate nothing after the first frames "
       "(static build)")
  {
    Registry registry;
    EventConsumer<LookupEvent> lookups;
    EventConsumer<LookupEvent> handled;
    EventConsumer<LookupLatestEvent> latestLookups;
    int count = 0;
    int runs = 0;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& p)
        {
          lookups = p.consumer<LookupEvent>();
          handled = p.consumer<LookupEvent>().onEvent([&](const LookupEvent&)
                                                      { ++count; });
          latestLookups = p.consumer<LookupLatestEvent>().onLatestEvents(
              [&](LatestEventRange<LookupLatestEvent> events)
              { count += static_cast<int>(events.size()); });
        });
    const Entity key = registry.createEntity();
    const auto once = [&]
    {
      registry.events().send(LookupEvent{1});
      registry.events().send(key, LookupLatestEvent{1});
      ++runs;
      run(registry,
          probe,
          [&](Probe&)
          {
            for (const LookupEvent& lookup : lookups.read())
              (void)lookup;

            for (const LookupEvent& lookup : lookups.read())
              (void)lookup;
          });
      frame(registry);
    };

    for (int i = 0; i < 4; ++i)
      once();

    if (!tracking())
      std::printf("      note: allocation tracking is off in this build\n");
    else
    {
      MemoryMetrics& metrics = getMemoryMetrics();
      const std::size_t before = metrics.allocationCount.load();
      {
        ScopedMemoryTracking track(MemoryTrackingPhase::Update);

        for (int i = 0; i < 100; ++i)
          once();
      }
      CHECK(metrics.allocationCount.load() == before);
    }

    // Each run's event, once per handler.
    CHECK(count == 2 * runs);
  }

  return testing::report("systemEventsTests");
}
