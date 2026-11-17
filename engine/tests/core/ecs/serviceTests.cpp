#include "../../testHarness.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/events/events.h"
#include "engine/core/events/responder.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/jobs/workerPool.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/allocationMetrics.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Services (docs/core/services): logic run only when its events bring it
// something, at the start of each step before the systems, working on the
// entities its events hand it.

using namespace sfs;

namespace
{

struct [[sfs::event_news]] PingEvent
{
  int value = 0;
};

struct [[sfs::event_news]] EchoEvent
{
  int value = 0;
};

struct [[sfs::event_entity_news]] TaskEvent
{
  int value = 0;
};

struct [[sfs::event_entity_news]] GuardIdleEvent
{
};

struct [[sfs::event_entity_news]] PatrolFinishedEvent
{
};

struct [[sfs::event_entity_news]] OrderEvent
{
  int value = 0;
};

struct [[sfs::event_news]] LookEvent
{
  Entity at;
};

struct [[sfs::event_news]] DisabledEvent
{
  int value = 0;
};

struct [[sfs::event_news]] LateHandleEvent
{
  int value = 0;
};

struct [[sfs::event_news]] SpawnEvent
{
  int value = 0;
};

struct [[sfs::event_response]] FoundResponse
{
  int value = 0;
};

struct [[sfs::event_response]] FailedResponse
{
  int value = 0;
};

struct [[sfs::event_response]] LegResponse
{
  int leg = 0;
};

struct [[sfs::event_request(FoundResponse,
                            progress = LegResponse,
                            failure = FailedResponse)]] SearchRequest
{
  int value = 0;
};

struct Health
{
  int value = 0;
};

struct Task
{
  int value = 0;
};

struct Guard
{
  int firstRoute = 0;
  int nextRoute = 0;
};

struct PatrolTask
{
  int route = 0;
  float progress = 0.0f;
};

struct Holder
{
  Entity target;
};

struct [[sfs::scene_data]] Clock
{
  double time = 0.0;
};

struct [[sfs::scene_data]] Tally
{
  int handled = 0;
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
    Logger::flush();
    int found = 0;

    for (const auto& [at, line] : lines)
      if (at == level && line.find(part) != std::string::npos)
        ++found;

    return found;
  }
};

// A service whose create() runs what a test gives it, with its verbs made
// public for the test. A test's handles are declared after its registry, so
// they go before the service that owns them.
class Host : public Service
{
public:
  explicit Host(std::function<void(Host&)> setUp) : m_setUp(std::move(setUp)) {}

  using Service::addSceneData;
  using Service::cancel;
  using Service::consumer;
  using Service::createEntity;
  using Service::destroyEntity;
  using Service::requester;
  using Service::sceneData;
  using Service::send;

  int creates = 0;

protected:
  void create() override
  {
    ++creates;

    if (m_setUp)
      m_setUp(*this);
  }

private:
  std::function<void(Host&)> m_setUp;
};

// Hosts of other types, so a registry can tell them apart in its order.
class HostB : public Host
{
  using Host::Host;
};
class HostC : public Host
{
  using Host::Host;
};

// A system whose create() and update() run what a test gives them, listing
// the entities with a Task, and counting those that join.
class Worker : public System
{
public:
  explicit Worker(std::function<void(Worker&)> setUp = {})
      : m_setUp(std::move(setUp))
  {
  }

  using EventVerbs::cancel;
  using EventVerbs::consumer;
  using EventVerbs::requester;
  using EventVerbs::send;

  std::function<void(Worker&)> body;
  int joined = 0;
  std::vector<Entity> seen;

  Registry& reg() { return *registry; }

protected:
  void create() override
  {
    registerComponent<Task>();

    if (m_setUp)
      m_setUp(*this);
  }
  void update(double) override
  {
    seen.clear();

    for (const Entity& entity : getEntities())
      seen.push_back(entity);

    if (body)
      body(*this);
  }
  void onEntityAdded(const Entity&) override { ++joined; }

private:
  std::function<void(Worker&)> m_setUp;
};

// Moves each guard along its patrol, telling when it's done.
class PatrolSystem : public System
{
public:
  using Entities = Query<Guard, PatrolTask>;
  std::vector<int> routes; // each step's routes, in id order

protected:
  void create() override { registerComponent<Entities>(); }
  void update(double) override
  {
    for (auto [guard, state, task] : getEntities<Entities>())
    {
      routes.push_back(task.route);
      const bool was = task.progress >= 1.0f;
      task.progress += 0.5f;

      if (!was && task.progress >= 1.0f)
        send(guard, PatrolFinishedEvent{});
    }
  }
};

// Set by the destruction test's system and read by its service.
std::atomic<bool> g_systemGone{false};
std::atomic<bool> g_systemGoneBeforeService{true};
std::atomic<bool> g_serverSawSuperseded{false};

class Server : public System
{
public:
  using EventVerbs::consumer;
  EventConsumer<SearchRequest> searches;
  std::vector<Responder<SearchRequest>> held;

  ~Server() override
  {
    g_systemGone = true;
    g_serverSawSuperseded = !held.empty() && held.front().superseded();
  }

protected:
  void create() override { searches = consumer<SearchRequest>(); }
  void update(double) override
  {
    for (auto [entity, search, responder] : searches.read())
      held.push_back(responder);
  }
};

class Leaving : public Service
{
public:
  using Service::requester;
  EventRequester<SearchRequest> asks;
  std::atomic<int> answered{0};

  ~Leaving() override
  {
    g_systemGoneBeforeService = g_systemGone.load();
    m_pool.reset();
  }

  void submit(const Responder<SearchRequest>& responder)
  {
    m_pool->submit(
        [responder, this]
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          responder.respond(FoundResponse{1});
          ++answered;
        });
  }

protected:
  void create() override
  {
    asks = requester<SearchRequest>()
               .onResponse([](const Entity&, const FoundResponse&) {})
               .onFailure([](const Entity&, const FailedResponse&) {});
    m_jobs = consumer<PingEvent>().onEvent([](const PingEvent&) {});
    m_pool = std::make_unique<WorkerPool>(1);
  }

private:
  EventConsumer<PingEvent> m_jobs;
  // Last: destroyed first, so its workers are joined before what a job holds
  // goes.
  std::unique_ptr<WorkerPool> m_pool;
};

// One step of a headless host: the frame boundary, then services before the
// systems.
void step(Registry& registry, std::initializer_list<System*> systems = {})
{
  registry.flushEntities();
  registry.events().update();
  registry.runServices();

  for (System* system : systems)
    registry.updateSystem(*system, 0.0);
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
  TEST("create() should run once; handlers only when a handle has something "
       "new")
  {
    Registry registry;
    EventConsumer<PingEvent> pings;
    int handled = 0;
    Host& host = registry.addService<Host>(
        [&](Host& h)
        {
          pings = h.consumer<PingEvent>().onEvent([&](const PingEvent&)
                                                  { ++handled; });
        });
    CHECK(host.creates == 1);
    step(registry);
    CHECK(handled == 0);
    CHECK(!host.hasUnconsumedEvents());
    registry.events().send(PingEvent{1});
    CHECK(host.hasUnconsumedEvents());
    step(registry);
    CHECK(handled == 1);
    CHECK(!host.hasUnconsumedEvents());
    step(registry);
    CHECK(handled == 1);
    CHECK(host.creates == 1);
  }

  TEST("a system's send should reach a service at its next run, and a "
       "service's changes a system in the same step")
  {
    Registry registry;
    EventConsumer<PingEvent> pings;
    EventConsumer<TaskEvent> tasks;
    std::vector<int> heard;
    Worker* hookedIn = nullptr;
    int joinedInHandler = -1;
    registry.addService<Host>(
        [&](Host& h)
        {
          pings = h.consumer<PingEvent>().onEvent(
              [&](const PingEvent& ping) { heard.push_back(ping.value); });
          tasks = h.consumer<TaskEvent>().onEvent(
              [&](const Entity& entity, const TaskEvent& task)
              {
                entity.addComponent<Task>(Task{task.value});
                joinedInHandler = hookedIn->joined;
              });
        });
    Worker& worker = registry.addSystem<Worker>();
    hookedIn = &worker;
    worker.body = [](Worker& w) { w.send(PingEvent{7}); };
    step(registry, {&worker});
    CHECK(heard.empty());
    step(registry);
    CHECK((heard == std::vector<int>{7}));

    const Entity entity = registry.createEntity();
    worker.body = {};
    step(registry, {&worker});
    registry.events().send(entity, TaskEvent{3});
    step(registry, {&worker});
    CHECK(joinedInHandler == 1);
    CHECK((worker.seen.size() == 1u && worker.seen[0] == entity));
    CHECK(entity.getComponent<Task>() &&
          entity.getComponent<Task>()->value == 3);
  }

  TEST("a coordinator should hand out a task, then the next by one add over "
       "the old one, one step between each")
  {
    Registry registry;
    EventConsumer<GuardIdleEvent> idle;
    EventConsumer<PatrolFinishedEvent> done;
    registry.addService<Host>(
        [&](Host& h)
        {
          idle = h.consumer<GuardIdleEvent>().onLatest(
              [](const Entity& guard, const GuardIdleEvent&)
              {
                if (const Guard* state = guard.getComponent<Guard>())
                  guard.addComponent<PatrolTask>(PatrolTask{state->firstRoute});
              });
          done = h.consumer<PatrolFinishedEvent>().onLatest(
              [](const Entity& guard, const PatrolFinishedEvent&)
              {
                const Guard* state = guard.getComponent<Guard>();

                if (!state)
                  return;

                guard.addComponent<PatrolTask>(PatrolTask{state->nextRoute});
              });
        });
    PatrolSystem& patrol = registry.addSystem<PatrolSystem>();
    const Entity guard = registry.createEntity();
    guard.addComponent<Guard>(Guard{1, 2});
    registry.events().send(guard, GuardIdleEvent{});

    for (int i = 0; i < 4; ++i)
      step(registry, {&patrol});

    // Route 1 twice (finished on the second), route 2 from the next step.
    CHECK((patrol.routes == std::vector<int>{1, 1, 2, 2}));
    CHECK(guard.getComponent<PatrolTask>()->progress >= 1.0f);
  }

  TEST("requests served by a service should be answered through the row's "
       "Responder, a worker's copy, or by entity")
  {
    Registry registry;
    EventConsumer<SearchRequest> searches;
    EventConsumer<OrderEvent> orders;
    EventRequester<SearchRequest> asker;
    std::vector<int> found;
    std::vector<int> failed;
    int legs = 0;
    std::atomic<bool> sawSuperseded{false};
    std::atomic<int> jobsDone{0};
    WorkerPool pool(1);
    registry.addService<Host>(
        [&](Host& h)
        {
          searches = h.consumer<SearchRequest>().onEvent(
              [&](const Entity&,
                  const SearchRequest& search,
                  const Responder<SearchRequest>& responder)
              {
                if (search.value == 1)
                  responder.respond(FoundResponse{1});
                else if (search.value == 2)
                  pool.submit(
                      [responder, &jobsDone]
                      {
                        responder.progress(LegResponse{1});
                        responder.respond(FoundResponse{2});
                        ++jobsDone;
                      });
                else if (search.value == 4)
                  pool.submit(
                      [responder, &sawSuperseded, &jobsDone]
                      {
                        while (!responder.superseded())
                          std::this_thread::sleep_for(
                              std::chrono::milliseconds(1));

                        sawSuperseded = true;
                        ++jobsDone;
                      });
              });
          orders = h.consumer<OrderEvent>().onEvent(
              [&](const Entity& entity, const OrderEvent&)
              { searches.fail(entity, FailedResponse{3}); });
        });
    Worker& worker = registry.addSystem<Worker>(
        [&](Worker& w)
        {
          asker = w.requester<SearchRequest>()
                      .onProgress([&](const Entity&, const LegResponse&)
                                  { ++legs; })
                      .onResponse([&](const Entity&, const FoundResponse& f)
                                  { found.push_back(f.value); })
                      .onFailure([&](const Entity&, const FailedResponse& f)
                                 { failed.push_back(f.value); });
        });
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    const Entity c = registry.createEntity();
    const Entity d = registry.createEntity();
    asker.request(a, SearchRequest{1});
    asker.request(b, SearchRequest{2});
    asker.request(c, SearchRequest{3});
    asker.request(d, SearchRequest{4});
    registry.events().send(c, OrderEvent{1});
    step(registry, {&worker});
    CHECK((found == std::vector<int>{1}));
    CHECK((failed == std::vector<int>{3}));

    for (int i = 0; i < 2000 && jobsDone.load() < 1; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

    registry.events().takeIn();
    registry.updateSystem(worker, 0.0);
    CHECK((found == std::vector<int>{1, 2}));
    CHECK(legs == 1);
    asker.cancel(d);

    for (int i = 0; i < 2000 && jobsDone.load() < 2; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

    CHECK(sawSuperseded.load());
  }

  TEST("a service's requester should hear its responses at its next run, a "
       "worker's included")
  {
    Registry registry;
    EventRequester<SearchRequest> asks;
    EventConsumer<SearchRequest> searches;
    std::vector<int> found;
    Host& host = registry.addService<Host>(
        [&](Host& h)
        {
          asks = h.requester<SearchRequest>()
                     .onResponse([&](const Entity&, const FoundResponse& f)
                                 { found.push_back(f.value); })
                     .onFailure([](const Entity&, const FailedResponse&) {});
        });
    std::vector<Responder<SearchRequest>> held;
    Worker& server = registry.addSystem<Worker>(
        [&](Worker& w) { searches = w.consumer<SearchRequest>(); });
    server.body = [&](Worker&)
    {
      for (auto [entity, search, responder] : searches.read())
        held.push_back(responder);
    };
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    asks.request(a, SearchRequest{1});
    asks.request(b, SearchRequest{2});
    step(registry, {&server});
    CHECK(held.size() == 2u);
    held[0].respond(FoundResponse{1});
    CHECK(host.hasUnconsumedEvents());
    step(registry);
    CHECK((found == std::vector<int>{1}));
    std::thread worker([&] { held[1].respond(FoundResponse{2}); });
    worker.join();
    registry.events().takeIn();
    registry.runServices();
    CHECK((found == std::vector<int>{1, 2}));
  }

  TEST("a service should read scene data and keep its own, which a system "
       "reads in the same step")
  {
    Registry registry;
    EventConsumer<PingEvent> pings;
    double readTime = -1.0;
    bool missing = false;
    Host& host = registry.addService<Host>(
        [&](Host& h)
        {
          missing = h.sceneData<Clock>() == nullptr;
          pings = h.consumer<PingEvent>().onEvent(
              [&h, &readTime](const PingEvent&)
              {
                static_assert(
                    std::is_same_v<decltype(h.sceneData<const Clock>()),
                                   const Clock*>);

                if (const Clock* clock = h.sceneData<const Clock>())
                  readTime = clock->time;

                if (Tally* tally = h.sceneData<Tally>())
                  ++tally->handled;
              });
        });
    CHECK(missing);
    registry.addSceneData<Clock>(Clock{4.5});
    Tally& tally = registry.addSceneData<Tally>();
    int systemRead = -1;
    Worker& worker = registry.addSystem<Worker>();
    worker.body = [&](Worker& w)
    {
      if (const Tally* tally = w.reg().sceneData<Tally>())
        systemRead = tally->handled;
    };
    registry.events().send(PingEvent{1});
    step(registry, {&worker});
    CHECK(readTime == 4.5);
    CHECK(systemRead == 1);
    CHECK(host.sceneData<Tally>() == &tally);
  }

  TEST("a service should create and destroy entities, and add its own scene "
       "data")
  {
    Registry registry;
    EventConsumer<SpawnEvent> spawns;
    std::vector<Entity> made;
    Host& host = registry.addService<Host>(
        [&](Host& h)
        {
          h.addSceneData<Tally>();
          spawns = h.consumer<SpawnEvent>().onEvent(
              [&](const SpawnEvent& spawn)
              {
                if (spawn.value > 0)
                  made.push_back(
                      h.createEntity().addComponent<Task>(Task{spawn.value}));
                else if (!made.empty())
                {
                  h.destroyEntity(made.front());
                  made.erase(made.begin());
                }

                ++h.sceneData<Tally>()->handled;
              });
        });
    CHECK(host.sceneData<Tally>() != nullptr);
    Worker& worker = registry.addSystem<Worker>();
    registry.events().send(SpawnEvent{5});
    step(registry, {&worker});
    // Made in this step's run: it joins at the next flush.
    CHECK(worker.seen.empty());
    step(registry, {&worker});
    CHECK((worker.seen.size() == 1u && worker.seen[0] == made.front()));
    const Entity first = made.front();
    registry.events().send(SpawnEvent{0});
    step(registry, {&worker});
    step(registry, {&worker});
    CHECK(!first.isValid());
    CHECK(worker.seen.empty());
    CHECK(host.sceneData<Tally>()->handled == 2);
  }

  TEST("a handler should be handed a dead entity safely: null, an add "
       "refused and logged, onLatest skipping it")
  {
    LogCapture log;
    Registry registry;
    EventConsumer<TaskEvent> every;
    EventConsumer<TaskEvent> latest;
    bool valid = true;
    bool read = true;
    int latestRuns = 0;
    registry.addService<Host>(
        [&](Host& h)
        {
          every = h.consumer<TaskEvent>().onEvent(
              [&](const Entity& entity, const TaskEvent&)
              {
                valid = entity.isValid();
                read = entity.getComponent<Health>() != nullptr;
                entity.addComponent<Task>();
              });
          latest = h.consumer<TaskEvent>().onLatest(
              [&](const Entity&, const TaskEvent&) { ++latestRuns; });
        });
    const Entity gone = registry.createEntity();
    gone.addComponent<Health>(Health{1});
    registry.events().send(gone, TaskEvent{1});
    registry.destroyEntity(gone);
    step(registry);
    CHECK(!valid);
    CHECK(!read);
    CHECK(latestRuns == 0);
    CHECK(log.count(Logger::Level::ERROR, "addComponent<") == 1);
  }

  TEST("a handler should reach what the event names and what its entities "
       "lead to")
  {
    Registry registry;
    EventConsumer<LookEvent> looks;
    std::vector<int> values;
    registry.addService<Host>(
        [&](Host& h)
        {
          looks = h.consumer<LookEvent>().onEvent(
              [&](const LookEvent& look)
              {
                if (const Health* health = look.at.getComponent<Health>())
                  values.push_back(health->value);

                for (const Entity& child : hierarchy::childrenOf(look.at))
                  if (const Health* health = child.getComponent<Health>())
                    values.push_back(health->value);

                if (const Holder* holder = look.at.getComponent<Holder>())
                  if (const Health* health =
                          holder->target.getComponent<Health>())
                    values.push_back(health->value);
              });
        });
    const Entity door = registry.createEntity();
    const Entity hinge = registry.createEntity();
    const Entity key = registry.createEntity();
    door.addComponent<Health>(Health{1});
    hinge.addComponent<Health>(Health{2});
    key.addComponent<Health>(Health{3});
    door.addComponent<Holder>(Holder{key});
    hierarchy::setParent(hinge, door);
    registry.events().send(LookEvent{door});
    step(registry);
    CHECK((values == std::vector<int>{1, 2, 3}));
  }

  TEST("a disabled service should miss nothing, and a paused registry still "
       "run it")
  {
    Registry registry;
    EventConsumer<DisabledEvent> events;
    std::vector<int> got;
    Host& host = registry.addService<Host>(
        [&](Host& h)
        {
          events = h.consumer<DisabledEvent>().onEvent(
              [&](const DisabledEvent& e) { got.push_back(e.value); });
        });
    host.setEnabled(false);

    for (int i = 0; i < 3; ++i)
    {
      registry.events().send(DisabledEvent{i});
      step(registry);
    }

    CHECK(got.empty());
    host.setEnabled(true);
    registry.setPaused(true);
    step(registry);
    CHECK((got == std::vector<int>{0, 1, 2}));
  }

  TEST("a handle made in a handler, or a handler given in one, should be "
       "refused and logged")
  {
    LogCapture log;
    Registry registry;
    EventConsumer<LateHandleEvent> triggers;
    EventConsumer<PingEvent> late;
    int lateRuns = 0;
    registry.addService<Host>(
        [&](Host& h)
        {
          triggers = h.consumer<LateHandleEvent>().onEvent(
              [&](const LateHandleEvent&)
              {
                late = h.consumer<PingEvent>().onEvent([&](const PingEvent&)
                                                       { ++lateRuns; });
                triggers.onEvent([&](const LateHandleEvent&) { ++lateRuns; });
              });
        });
    registry.events().send(LateHandleEvent{1});
    step(registry);
    registry.events().send(PingEvent{1});
    registry.events().send(LateHandleEvent{2});
    step(registry);
    CHECK(lateRuns == 0);
    CHECK(log.count(Logger::Level::ERROR, "outside create()") == 1);
    CHECK(log.count(Logger::Level::ERROR, "while its owner's handlers ran") ==
          1);
  }

  TEST("services should go before the systems and the events, their workers "
       "joined, their requests superseded for their servers")
  {
    g_systemGone = false;
    g_systemGoneBeforeService = true;
    g_serverSawSuperseded = false;
    {
      Registry registry;
      Leaving& leaving = registry.addService<Leaving>();
      Server& server = registry.addSystem<Server>();
      const Entity asked = registry.createEntity();
      leaving.asks.request(asked, SearchRequest{1});
      step(registry, {&server});
      CHECK(server.held.size() == 1u);
      leaving.submit(server.held.front());
    }
    CHECK(!g_systemGoneBeforeService.load());
    CHECK(g_serverSawSuperseded.load());
  }

  TEST("a service added after a sender should hear it in the same run, one "
       "added before it in the next")
  {
    Registry registry;
    EventConsumer<EchoEvent> before;
    EventConsumer<PingEvent> pings;
    EventConsumer<EchoEvent> after;
    int beforeHeard = 0;
    int afterHeard = 0;
    registry.addService<HostB>(
        [&](Host& h)
        {
          before = h.consumer<EchoEvent>().onEvent([&](const EchoEvent&)
                                                   { ++beforeHeard; });
        });
    registry.addService<Host>(
        [&](Host& h)
        {
          pings = h.consumer<PingEvent>().onEvent(
              [&h](const PingEvent& ping) { h.send(EchoEvent{ping.value}); });
        });
    registry.addService<HostC>(
        [&](Host& h)
        {
          after = h.consumer<EchoEvent>().onEvent([&](const EchoEvent&)
                                                  { ++afterHeard; });
        });
    registry.events().send(PingEvent{1});
    step(registry);
    CHECK(afterHeard == 1);
    CHECK(beforeHeard == 0);
    step(registry);
    CHECK(beforeHeard == 1);
  }

  TEST("cancel should take back entity news from a service and a system")
  {
    Registry registry;
    EventConsumer<TaskEvent> triggers;
    EventConsumer<OrderEvent> orders;
    std::vector<int> latest;
    registry.addService<Host>(
        [&](Host& h)
        {
          triggers = h.consumer<TaskEvent>().onEvent(
              [&h](const Entity& entity, const TaskEvent&)
              { h.cancel<OrderEvent>(entity); });
        });
    Worker& worker = registry.addSystem<Worker>(
        [&](Worker& w) { orders = w.consumer<OrderEvent>(); });
    worker.body = [&](Worker&)
    {
      for (auto [entity, order] : orders.readLatest())
        latest.push_back(order.value);
    };
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.events().send(a, OrderEvent{1});
    registry.events().send(b, OrderEvent{2});
    registry.events().send(a, TaskEvent{1});
    step(registry, {&worker});
    CHECK((latest == std::vector<int>{2}));

    latest.clear();
    registry.events().send(b, OrderEvent{3});
    Worker& canceller = registry.addSystem<Worker>();
    canceller.body = [&](Worker& w) { w.cancel<OrderEvent>(b); };
    step(registry, {&canceller, &worker});
    CHECK(latest.empty());
  }

  TEST("services should allocate nothing once warm (static build)")
  {
    Registry registry;
    EventConsumer<PingEvent> pings[4];
    EventConsumer<TaskEvent> tasks[4];
    int handled = 0;
    registry.addService<Host>(
        [&](Host& h)
        {
          pings[0] = h.consumer<PingEvent>().onEvent([&](const PingEvent&)
                                                     { ++handled; });
          tasks[0] = h.consumer<TaskEvent>().onLatest(
              [&](const Entity& e, const TaskEvent& t)
              { e.addComponent<Task>(Task{t.value}); });
        });
    registry.addService<HostB>(
        [&](Host& h)
        {
          pings[1] = h.consumer<PingEvent>().onEvent([&](const PingEvent&)
                                                     { ++handled; });
          tasks[1] = h.consumer<TaskEvent>().onEvents(
              [&](EventRange<TaskEvent> events)
              { handled += static_cast<int>(events.size()); });
        });
    registry.addService<HostC>(
        [&](Host& h)
        {
          pings[2] = h.consumer<PingEvent>().onEvent(
              [&h](const PingEvent& p) { h.send(EchoEvent{p.value}); });
        });
    EventConsumer<EchoEvent> echoes;
    registry.addService<Host>(
        [&](Host& h)
        {
          echoes = h.consumer<EchoEvent>().onEvent([&](const EchoEvent&)
                                                   { ++handled; });
        });
    Worker& worker = registry.addSystem<Worker>();
    const Entity key = registry.createEntity();
    const auto once = [&]
    {
      registry.events().send(PingEvent{1});
      registry.events().send(key, TaskEvent{1});
      step(registry, {&worker});
    };

    for (int i = 0; i < 8; ++i)
      once();

    if (!tracking())
      std::printf("      note: allocation tracking is off in this build\n");
    else
    {
      MemoryMetrics& metrics = getMemoryMetrics();
      const std::size_t before = metrics.allocationCount.load();
      {
        ScopedMemoryTracking track(MemoryTrackingPhase::Update);

        for (int i = 0; i < 1000; ++i)
          once();
      }
      CHECK(metrics.allocationCount.load() == before);
    }

    CHECK(handled > 0);
  }

  return testing::report("serviceTests");
}
