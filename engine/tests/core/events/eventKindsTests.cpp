#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>
#include <engine/core/events/events.h>
#include <engine/core/events/responder.h>
#include <engine/core/logger/logger.h>
#include <engine/core/util/debugFeatures.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Event kinds: entity news and its generations, readLatest, requests
// through requesters, responders and responses (docs/core/events).

using namespace sfs;

namespace
{

struct [[sfs::event_entity_news]] MovedEvent
{
  int value = 0;
};

struct [[sfs::event_response]] RouteLegResponse
{
  int leg = 0;
};

struct [[sfs::event_response]] RouteDoneResponse
{
  int outcome = 0;
};

struct [[sfs::event_request(RouteLegResponse, RouteDoneResponse)]] RouteRequest
{
  int ask = 0;
};

// A second request that RouteDoneResponse also responses.
struct [[sfs::event_request(RouteDoneResponse)]] DetourRequest
{
  int ask = 0;
};

// Responded only by workers before anyone handles it.
struct [[sfs::event_response]] PlannedResponse
{
  int result = 0;
};
struct [[sfs::event_request(PlannedResponse)]] PlanRequest
{
  int ask = 0;
};

// For the threaded test alone.
struct [[sfs::event_response]] BusyResponse
{
  int ask = 0;
};
struct [[sfs::event_request(BusyResponse)]] BusyRequest
{
  int ask = 0;
};

// Keyed news a worker sends.
struct [[sfs::event_entity_news]] FoundEvent
{
  int value = 0;
};

struct [[sfs::event_news]] NewsEvent
{
  int value = 0;
};

struct [[sfs::event_entity_news]] LoggedKeyedEvent
{
  int value = 0;
};

// For the stale handle test.
struct [[sfs::event_response]] StaleDoneResponse
{
  int outcome = 0;
};
struct [[sfs::event_request(StaleDoneResponse)]] StaleRequest
{
  int ask = 0;
};

// An event family: news, news derived from it, and news derived from that.
struct [[sfs::event_news]] DamageEvent
{
  int amount = 0;
};
struct [[sfs::event_news]] FireDamageEvent : DamageEvent
{
  int burn = 0;
};
struct [[sfs::event_news]] InfernoDamageEvent : FireDamageEvent
{
  int spread = 0;
};

// Entity news in a family.
struct [[sfs::event_entity_news]] HitEvent
{
  int amount = 0;
};
struct [[sfs::event_entity_news]] CriticalHitEvent : HitEvent
{
  int multiplier = 0;
};

// A request two systems ask, for the shared-request tests.
struct [[sfs::event_response]] LookFoundResponse
{
  int value = 0;
};
struct [[sfs::event_request(LookFoundResponse)]] LookRequest
{
  int value = 0;
};

// Two response types, for the order test.
struct [[sfs::event_response]] StepResponse
{
  int step = 0;
};
struct [[sfs::event_response]] ArrivedResponse
{
  int at = 0;
};
struct [[sfs::event_request(StepResponse, ArrivedResponse)]] TravelRequest
{
  int to = 0;
};

// Entity news sent about an invalid entity.
struct [[sfs::event_entity_news]] NowhereEvent
{
  int value = 0;
};

static_assert(std::is_same_v<detail::EventTraits<InfernoDamageEvent>::Ancestors,
                             detail::TypeList<FireDamageEvent, DamageEvent>>);

static_assert(detail::kKeyed<MovedEvent> && !detail::kRequest<MovedEvent>);
static_assert(detail::kKeyed<RouteRequest> && detail::kRequest<RouteRequest>);
static_assert(detail::kResponse<RouteLegResponse> &&
              detail::kKeyed<RouteLegResponse> &&
              !detail::kOwnChannel<RouteLegResponse>);
static_assert(
    detail::kKeyed<detail::ResponseTo<RouteRequest, RouteLegResponse>>);
static_assert(!detail::kKeyed<NewsEvent> && !detail::kRequest<NewsEvent>);

// Every range a consumer or a requester's handler gets is a forward range, so
// std::ranges algorithms take it.
static_assert(std::forward_iterator<EventRange<NewsEvent>::iterator>);
static_assert(std::forward_iterator<EventRange<MovedEvent>::iterator>);
static_assert(std::forward_iterator<LatestEventRange<MovedEvent>::iterator>);
static_assert(std::forward_iterator<EventRange<RouteRequest>::iterator>);
static_assert(std::forward_iterator<
              ResponseRange<RouteRequest, RouteLegResponse>::iterator>);
static_assert(std::ranges::forward_range<EventRange<NewsEvent>>);
static_assert(std::ranges::forward_range<const EventRange<MovedEvent>&>);
static_assert(
    std::ranges::forward_range<ResponseRange<RouteRequest, RouteLegResponse>>);
static_assert(std::is_same_v<std::iter_value_t<EventRange<NewsEvent>::iterator>,
                             NewsEvent>);

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

// A system whose create() and update() run what a test gives them, with its
// event calls made public for the test.
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

protected:
  void create() override
  {
    if (m_setUp)
      m_setUp(*this);
  }
  void update(double) override
  {
    if (body)
      body(*this);
  }

private:
  std::function<void(Probe&)> m_setUp;
};

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

} // namespace

int main()
{
  TEST("read of a keyed type should give every event with its entity")
  {
    Registry registry;
    EventConsumer<MovedEvent> moved;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self) { moved = self.consumer<MovedEvent>(); });
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.events().send(a, MovedEvent{1});
    registry.events().send(b, MovedEvent{2});
    registry.events().send(a, MovedEvent{3});
    std::vector<std::pair<Entity, int>> seen;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, event] : moved.read())
            seen.emplace_back(entity, event.value);
        });
    CHECK(seen.size() == 3u);
    CHECK((seen[0] == std::pair<Entity, int>{a, 1}));
    CHECK((seen[2] == std::pair<Entity, int>{a, 3}));
    // A keyed row converts to its event.
    int sum = 0;
    registry.events().send(b, MovedEvent{4});
    run(registry,
        probe,
        [&](Probe&)
        {
          for (const MovedEvent& event : moved.read())
            sum += event.value;
        });
    CHECK(sum == 4);
  }

  TEST("a read should work with std::ranges algorithms, keyed or not")
  {
    Registry registry;
    EventConsumer<MovedEvent> moved;
    EventConsumer<NewsEvent> news;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          moved = self.consumer<MovedEvent>();
          news = self.consumer<NewsEvent>();
        });
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.events().send(a, MovedEvent{1});
    registry.events().send(b, MovedEvent{2});
    registry.events().send(NewsEvent{3});
    registry.events().send(NewsEvent{4});
    bool movedB = false;
    bool movedC = true;
    std::ptrdiff_t big = 0;
    run(registry,
        probe,
        [&](Probe&)
        {
          const auto rows = moved.read();
          movedB = std::ranges::any_of(rows,
                                       [&](const KeyedRow<MovedEvent>& row)
                                       { return row.entity == b; });
          movedC = std::ranges::any_of(
              rows, [](const MovedEvent& event) { return event.value == 3; });
          big = std::ranges::count_if(news.read(),
                                      [](const NewsEvent& event)
                                      { return event.value > 3; });
        });
    CHECK(movedB);
    CHECK(!movedC);
    CHECK(big == 1);
  }

  TEST("readLatest should give each key's newest event, for every reader")
  {
    Registry registry;
    EventConsumer<MovedEvent> first;
    EventConsumer<MovedEvent> second;
    Probe& one = registry.addSystem<Probe>(
        [&](Probe& self) { first = self.consumer<MovedEvent>(); });
    Probe& two = registry.addSystem<Probe>(
        [&](Probe& self) { second = self.consumer<MovedEvent>(); });
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.events().send(a, MovedEvent{1});
    registry.events().send(b, MovedEvent{2});
    registry.events().send(a, MovedEvent{3});
    const auto latest = [&](Probe& probe, EventConsumer<MovedEvent>& consumer)
    {
      std::vector<int> values;
      run(registry,
          probe,
          [&](Probe&)
          {
            for (auto [entity, event] : consumer.readLatest())
              values.push_back(event.value);
          });
      return values;
    };
    CHECK((latest(one, first) == std::vector<int>{2, 3}));
    CHECK((latest(two, second) == std::vector<int>{2, 3}));
    // A newer one next frame is given; the one already seen isn't again.
    frame(registry);
    registry.events().send(b, MovedEvent{4});
    CHECK((latest(one, first) == std::vector<int>{4}));
  }

  TEST("cancel should supersede a key's current event")
  {
    Registry registry;
    EventConsumer<MovedEvent> all;
    EventConsumer<MovedEvent> latest;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          all = self.consumer<MovedEvent>();
          latest = self.consumer<MovedEvent>();
        });
    const Entity a = registry.createEntity();
    registry.events().send(a, MovedEvent{1});
    registry.events().cancel<MovedEvent>(a);
    std::size_t latestCount = 0;
    std::size_t allCount = 0;
    run(registry,
        probe,
        [&](Probe&)
        {
          latestCount = latest.readLatest().size();
          allCount = all.read().size();
        });
    CHECK(latestCount == 0u);
    CHECK(allCount == 1u);
  }

  TEST("a destroyed key should supersede its events, and a reused id never "
       "match them")
  {
    Registry registry;
    EventConsumer<MovedEvent> moved;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self) { moved = self.consumer<MovedEvent>(); });
    registry.flushEntities();
    const Entity a = registry.createEntity();
    registry.flushEntities();
    registry.events().send(a, MovedEvent{1});
    registry.destroyEntity(a);
    frame(registry);
    const Entity reused = registry.createEntity();
    CHECK(reused.getId() == a.getId());
    std::size_t latest = 1;
    run(registry, probe, [&](Probe&) { latest = moved.readLatest().size(); });
    CHECK(latest == 0u);
    registry.events().send(reused, MovedEvent{2});
    std::vector<Entity> keys;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, event] : moved.readLatest())
            keys.push_back(entity);
        });
    CHECK((keys.size() == 1u && keys[0] == reused));
    std::vector<EventStats> stats;
    registry.events().stats(stats);
    CHECK(!stats.empty() && stats[0].superseded == 1u);
  }

  TEST("a requester should get its responses in its owner's slot, progress "
       "then final, and waiting() follow the final response")
  {
    Registry registry;
    EventRequester<RouteRequest> routes;
    EventConsumer<RouteRequest> asked;
    std::vector<int> legs;
    std::vector<int> outcomes;
    Probe& asker = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          routes = self.requester<RouteRequest>()
                       .onResponse<RouteLegResponse>(
                           [&](const Entity&, const RouteLegResponse& leg)
                           { legs.push_back(leg.leg); })
                       .onResponse<RouteDoneResponse>(
                           [&](const Entity&, const RouteDoneResponse& done)
                           { outcomes.push_back(done.outcome); });
        });
    std::vector<Responder<RouteRequest>> responders;
    Probe& planner = registry.addSystem<Probe>(
        [&](Probe& self) { asked = self.consumer<RouteRequest>(); });
    const Entity vehicle = registry.createEntity();
    routes.request(vehicle, RouteRequest{1});
    CHECK(routes.waiting(vehicle));
    run(registry,
        planner,
        [&](Probe&)
        {
          for (auto [entity, route, responder] : asked.read())
          {
            CHECK(entity == vehicle);
            CHECK(route.ask == 1);
            CHECK(responder.key() == vehicle);
            CHECK(!responder.superseded());
            CHECK(responder.progress(RouteLegResponse{10}));
            responders.push_back(responder);
          }
        });
    CHECK(responders.size() == 1u);
    CHECK(routes.waiting(vehicle));
    run(registry, asker);
    CHECK((legs == std::vector<int>{10}));
    CHECK(outcomes.empty());
    CHECK(!responders.empty() && responders[0].respond(RouteDoneResponse{7}));
    CHECK(!routes.waiting(vehicle));
    run(registry, asker);
    CHECK((outcomes == std::vector<int>{7}));
    // Nothing new: the handlers aren't called.
    run(registry, asker);
    CHECK(outcomes.size() == 1u);
  }

  TEST("a newer request should supersede the older's responder and responses")
  {
    Registry registry;
    EventRequester<RouteRequest> routes;
    EventConsumer<RouteRequest> asked;
    std::vector<int> outcomes;
    Probe& asker = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          routes = self.requester<RouteRequest>()
                       .onResponse<RouteDoneResponse>(
                           [&](const Entity&, const RouteDoneResponse& done)
                           { outcomes.push_back(done.outcome); })
                       .ignore<RouteLegResponse>();
        });
    Probe& planner = registry.addSystem<Probe>(
        [&](Probe& self) { asked = self.consumer<RouteRequest>(); });
    const Entity vehicle = registry.createEntity();
    std::vector<Responder<RouteRequest>> responders;
    const auto takeAll = [&]
    {
      run(registry,
          planner,
          [&](Probe&)
          {
            for (auto [entity, route, responder] : asked.readLatest())
              responders.push_back(std::move(responder));
          });
    };
    routes.request(vehicle, RouteRequest{1});
    takeAll();
    routes.request(vehicle, RouteRequest{2});
    takeAll();
    CHECK(responders.size() == 2u);
    CHECK(responders[0].superseded());
    CHECK(!responders[1].superseded());
    // The newer responses first; the older's late response is dropped.
    CHECK(responders[1].respond(RouteDoneResponse{2}));
    CHECK(!responders[0].respond(RouteDoneResponse{1}));
    run(registry, asker);
    CHECK((outcomes == std::vector<int>{2}));
    // A response kept, then a newer request before the asker's slot: not
    // given.
    responders[1].progress(RouteDoneResponse{3});
    routes.request(vehicle, RouteRequest{3});
    outcomes.clear();
    run(registry, asker);
    CHECK(outcomes.empty());
    CHECK(routes.waiting(vehicle));
    routes.cancel(vehicle);
    CHECK(!routes.waiting(vehicle));
  }

  TEST("a destroyed entity's request should get no response")
  {
    Registry registry;
    EventRequester<RouteRequest> routes;
    EventConsumer<RouteRequest> asked;
    int responses = 0;
    Probe& asker = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          routes = self.requester<RouteRequest>()
                       .onResponse<RouteDoneResponse>(
                           [&](const Entity&, const RouteDoneResponse&)
                           { ++responses; })
                       .ignore<RouteLegResponse>();
        });
    Probe& planner = registry.addSystem<Probe>(
        [&](Probe& self) { asked = self.consumer<RouteRequest>(); });
    registry.flushEntities();
    const Entity vehicle = registry.createEntity();
    registry.flushEntities();
    routes.request(vehicle, RouteRequest{1});
    Responder<RouteRequest> kept;
    run(registry,
        planner,
        [&](Probe&)
        {
          for (auto [entity, route, responder] : asked.read())
            kept = responder;
        });
    registry.destroyEntity(vehicle);
    frame(registry);
    CHECK(kept.superseded());
    CHECK(!kept.respond(RouteDoneResponse{1}));
    run(registry, asker);
    CHECK(responses == 0);
  }

  TEST("workers' responses should be taken in before the next step, and "
       "dropped "
       "once superseded")
  {
    Registry registry;
    EventRequester<RouteRequest> routes;
    EventConsumer<RouteRequest> asked;
    std::vector<std::pair<Entity, int>> outcomes;
    int legs = 0;
    Probe& asker = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          routes =
              self.requester<RouteRequest>()
                  .onResponse<RouteLegResponse>(
                      [&](const Entity&, const RouteLegResponse&) { ++legs; })
                  .onResponses<RouteDoneResponse>(
                      [&](ResponseRange<RouteRequest, RouteDoneResponse>
                              responses)
                      {
                        for (auto [entity, done] : responses)
                          outcomes.emplace_back(entity, done.outcome);
                      });
        });
    Probe& planner = registry.addSystem<Probe>(
        [&](Probe& self) { asked = self.consumer<RouteRequest>(); });
    const Entity vehicle = registry.createEntity();
    const Entity other = registry.createEntity();
    routes.request(vehicle, RouteRequest{1});
    routes.request(other, RouteRequest{1});
    std::vector<Responder<RouteRequest>> responders;
    run(registry,
        planner,
        [&](Probe&)
        {
          for (auto [entity, route, responder] : asked.read())
            responders.push_back(std::move(responder));
        });
    CHECK(responders.size() == 2u);
    std::thread worker(
        [&]
        {
          CHECK(responders[0].progress(RouteLegResponse{1}));
          CHECK(responders[0].respond(RouteDoneResponse{1}));
          CHECK(responders[1].respond(RouteDoneResponse{2}));
        });
    worker.join();
    // Staged until taken in; the other key's request superseded meanwhile.
    CHECK(routes.waiting(vehicle));
    routes.request(other, RouteRequest{2});
    registry.events().takeIn();
    CHECK(!routes.waiting(vehicle));
    CHECK(routes.waiting(other));
    run(registry, asker);
    CHECK(legs == 1);
    CHECK((outcomes.size() == 1u &&
           outcomes[0] == std::pair<Entity, int>{vehicle, 1}));
    std::vector<EventStats> stats;
    registry.events().stats(stats);
    uint64_t dropped = 0;
    for (const EventStats& type : stats)
      if (type.type.find("RouteDoneResponse (response to") != std::string::npos)
        dropped = type.dropped;
    CHECK(dropped == 1u);
  }

  TEST("a worker's response before its queue exists should still arrive")
  {
    Registry registry;
    EventConsumer<PlanRequest> plans;
    EventRequester<PlanRequest> asker;
    Probe& planner = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          plans = self.consumer<PlanRequest>();
          // Ignored: nothing makes the response type's queue yet.
          asker = self.requester<PlanRequest>().ignore<PlannedResponse>();
        });
    const Entity vehicle = registry.createEntity();
    asker.request(vehicle, PlanRequest{1});
    Responder<PlanRequest> responder;
    run(registry,
        planner,
        [&](Probe&)
        {
          for (auto [entity, plan, taken] : plans.read())
            responder = taken;
        });
    CHECK(asker.waiting(vehicle));
    std::thread worker([&] { CHECK(responder.respond(PlannedResponse{5})); });
    worker.join();
    // Taken in at the boundary, its queue made for it: the request's final
    // response arrived.
    frame(registry);
    CHECK(!asker.waiting(vehicle));
    std::vector<EventStats> stats;
    registry.events().stats(stats);
    bool made = false;
    for (const EventStats& type : stats)
      made = made || type.type.find("PlannedResponse (response to") !=
                         std::string::npos;
    CHECK(made);
  }

  TEST("a responder should fail once its events are gone, and a default "
       "responder "
       "always")
  {
    Responder<RouteRequest> kept;
    {
      Registry registry;
      EventConsumer<RouteRequest> asked;
      EventRequester<RouteRequest> asker;
      Probe& planner = registry.addSystem<Probe>(
          [&](Probe& self)
          {
            asked = self.consumer<RouteRequest>();
            asker = self.requester<RouteRequest>()
                        .ignore<RouteLegResponse>()
                        .ignore<RouteDoneResponse>();
          });
      const Entity vehicle = registry.createEntity();
      asker.request(vehicle, RouteRequest{1});
      run(registry,
          planner,
          [&](Probe&)
          {
            for (auto [entity, route, responder] : asked.read())
              kept = responder;
          });
      CHECK(!kept.superseded());
    }
    CHECK(kept.superseded());
    CHECK(!kept.respond(RouteDoneResponse{1}));
    std::thread worker([&] { CHECK(!kept.respond(RouteDoneResponse{1})); });
    worker.join();
    const Responder<RouteRequest> none;
    CHECK(none.superseded());
    CHECK(!none.respond(RouteDoneResponse{1}));
  }

  TEST("a moved-from responder should hold nothing and do nothing")
  {
    Responder<RouteRequest> moved;
    Responder<RouteRequest> kept;
    {
      Registry registry;
      EventConsumer<RouteRequest> asked;
      EventRequester<RouteRequest> asker;
      Probe& planner = registry.addSystem<Probe>(
          [&](Probe& self)
          {
            asked = self.consumer<RouteRequest>();
            asker = self.requester<RouteRequest>()
                        .ignore<RouteLegResponse>()
                        .ignore<RouteDoneResponse>();
          });
      const Entity vehicle = registry.createEntity();
      asker.request(vehicle, RouteRequest{1});
      run(registry,
          planner,
          [&](Probe&)
          {
            for (auto [entity, route, responder] : asked.read())
              moved = responder;
          });
      kept = std::move(moved);
      CHECK(moved.superseded());
      CHECK(!moved.respond(RouteDoneResponse{1}));
      CHECK(!kept.superseded());
      Responder<RouteRequest> again;
      again = std::move(kept);
      CHECK(kept.superseded());
      kept = std::move(again);
    }
    CHECK(moved.superseded());
    CHECK(kept.superseded());
    CHECK(!moved.respond(RouteDoneResponse{1}));
    CHECK(!kept.respond(RouteDoneResponse{1}));
  }

  TEST("a worker's keyed news should get its generation as it's taken in")
  {
    Registry registry;
    EventConsumer<FoundEvent> all;
    EventConsumer<FoundEvent> latest;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          all = self.consumer<FoundEvent>();
          latest = self.consumer<FoundEvent>();
        });
    const Entity a = registry.createEntity();
    EventBroadcast<FoundEvent> broadcast =
        registry.events().broadcast<FoundEvent>();
    std::thread worker(
        [&]
        {
          CHECK(broadcast.send(a, FoundEvent{1}));
          CHECK(broadcast.send(a, FoundEvent{2}));
        });
    worker.join();
    frame(registry);
    std::vector<int> newest;
    std::size_t count = 0;
    run(registry,
        probe,
        [&](Probe&)
        {
          count = all.read().size();
          for (const FoundEvent& found : latest.readLatest())
            newest.push_back(found.value);
        });
    CHECK(count == 2u);
    CHECK((newest == std::vector<int>{2}));
  }

  TEST("a worker's keyed news for an id past the pages should be dropped, "
       "as on the owning thread")
  {
    Registry registry;
    EventConsumer<FoundEvent> found;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self) { found = self.consumer<FoundEvent>(); });
    LogCapture log;
    EventBroadcast<FoundEvent> broadcast =
        registry.events().broadcast<FoundEvent>();
    const Entity far(2'000'000u, 0u, 0u);
    std::thread worker([&] { CHECK(broadcast.send(far, FoundEvent{1})); });
    worker.join();
    frame(registry);
    std::size_t count = 1;
    run(registry, probe, [&](Probe&) { count = found.read().size(); });
    CHECK(count == 0u);
    CHECK(log.count(Logger::Level::ERROR, "keyed events take ids below") == 1);
  }

  TEST("workers' keyed news racing the owner's frame boundaries should all "
       "arrive")
  {
    Registry registry;
    EventConsumer<FoundEvent> found;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self) { found = self.consumer<FoundEvent>(); });
    const Entity key = registry.createEntity();
    EventBroadcast<FoundEvent> broadcast =
        registry.events().broadcast<FoundEvent>();
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
      workers.emplace_back(
          [&]
          {
            while (!go.load())
              std::this_thread::yield();
            for (int i = 0; i < 500; ++i)
              broadcast.send(key, FoundEvent{i});
          });
    go.store(true);
    std::size_t all = 0;
    for (int round = 0; round < 50; ++round)
    {
      registry.events().takeIn();
      frame(registry);
      run(registry, probe, [&](Probe&) { all += found.read().size(); });
    }
    for (std::thread& worker : workers)
      worker.join();
    frame(registry);
    run(registry, probe, [&](Probe&) { all += found.read().size(); });
    CHECK(all == 2000u);
  }

  TEST("4 threads responding while the owner supersedes should keep only "
       "current responses")
  {
    Registry registry;
    EventConsumer<BusyRequest> asked;
    EventRequester<BusyRequest> busy;
    int stale = 0;
    std::vector<Entity> keys;
    Probe& planner = registry.addSystem<Probe>(
        [&](Probe& self) { asked = self.consumer<BusyRequest>(); });
    Probe& asker = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          busy = self.requester<BusyRequest>().onResponse<BusyResponse>(
              [&](const Entity& entity, const BusyResponse&)
              { stale += entity == keys[0] ? 1 : 0; });
        });
    for (int i = 0; i < 4; ++i)
      keys.push_back(registry.createEntity());
    for (const Entity& key : keys)
      busy.request(key, BusyRequest{0});
    std::vector<Responder<BusyRequest>> responders;
    run(registry,
        planner,
        [&](Probe&)
        {
          for (auto [entity, request, responder] : asked.read())
            responders.push_back(std::move(responder));
        });
    run(registry, asker);
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
      workers.emplace_back(
          [&, t]
          {
            while (!go.load())
              std::this_thread::yield();
            for (int i = 0; i < 2000 && !responders[t].superseded(); ++i)
              responders[t].progress(BusyResponse{t});
          });
    go.store(true);
    for (int round = 0; round < 50; ++round)
    {
      registry.events().takeIn();
      if (round == 10)
        busy.request(keys[0], BusyRequest{1});
    }
    for (std::thread& worker : workers)
      worker.join();
    // Only responses taken in after the newer request count as stale.
    stale = 0;
    frame(registry);
    for (int f = 0; f < 2; ++f)
    {
      run(registry, asker);
      frame(registry);
    }
    CHECK(stale == 0);
  }

  TEST("a keyed send, request or cancel through a stale handle should be "
       "refused, sparing the entity that reuses its id")
  {
    Registry registry;
    EventConsumer<StaleRequest> asked;
    EventRequester<StaleRequest> stale;
    Probe& planner = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          asked = self.consumer<StaleRequest>();
          stale = self.requester<StaleRequest>().ignore<StaleDoneResponse>();
        });
    LogCapture log;
    const Entity old = registry.createEntity();
    registry.flushEntities();
    registry.destroyEntity(old);
    frame(registry);
    const Entity reused = registry.createEntity();
    CHECK(reused.getId() == old.getId());
    stale.request(reused, StaleRequest{1});
    std::vector<Responder<StaleRequest>> responders;
    run(registry,
        planner,
        [&](Probe&)
        {
          for (auto [entity, request, responder] : asked.readLatest())
            responders.push_back(responder);
        });
    CHECK(responders.size() == 1u);
    stale.cancel(old);
    stale.request(old, StaleRequest{2});
    CHECK((!responders.empty() && !responders[0].superseded()));
    CHECK(stale.waiting(reused));
    CHECK(!stale.waiting(old));
    std::size_t taken = 1;
    run(registry, planner, [&](Probe&) { taken = asked.readLatest().size(); });
    CHECK(taken == 0u);
    CHECK(log.count(Logger::Level::WARN,
                    "its handle is to an entity that "
                    "was destroyed") == 1);
  }

  TEST("a response type's consumers should see every response kept, to every "
       "request, while requesters see only their own request's")
  {
    Registry registry;
    EventConsumer<RouteRequest> routes;
    EventConsumer<DetourRequest> detours;
    EventConsumer<RouteDoneResponse> watched;
    EventRequester<RouteRequest> router;
    EventRequester<DetourRequest> detourer;
    int routed = 0;
    int detoured = 0;
    Probe& planner = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          routes = self.consumer<RouteRequest>();
          detours = self.consumer<DetourRequest>();
        });
    Probe& asker = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          router = self.requester<RouteRequest>()
                       .onResponse<RouteDoneResponse>(
                           [&](const Entity&, const RouteDoneResponse&)
                           { ++routed; })
                       .ignore<RouteLegResponse>();
          detourer =
              self.requester<DetourRequest>().onResponse<RouteDoneResponse>(
                  [&](const Entity&, const RouteDoneResponse&) { ++detoured; });
        });
    std::vector<int> seen;
    std::vector<Entity> keys;
    Probe& watcher = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          watched = self.consumer<RouteDoneResponse>().onEvent(
              [&](const Entity& key, const RouteDoneResponse& done)
              {
                keys.push_back(key);
                seen.push_back(done.outcome);
              });
        });
    const Entity a = registry.createEntity();
    router.request(a, RouteRequest{1});
    detourer.request(a, DetourRequest{1});
    run(registry,
        planner,
        [&](Probe&)
        {
          for (auto [entity, route, responder] : routes.read())
          {
            responder.progress(RouteLegResponse{1});
            responder.respond(RouteDoneResponse{1});
          }
          for (auto [entity, detour, responder] : detours.read())
            responder.respond(RouteDoneResponse{2});
        });
    run(registry, asker);
    run(registry, watcher);
    CHECK(routed == 1);
    CHECK(detoured == 1);
    CHECK((seen == std::vector<int>{1, 2}));
    CHECK(keys.size() == 2u && keys[0] == a && keys[1] == a);
  }

  TEST("news should reach its event bases' consumers, one and two levels "
       "up, in send order with the bases' own sends")
  {
    Registry registry;
    EventConsumer<DamageEvent> damage;
    EventConsumer<FireDamageEvent> fire;
    EventConsumer<InfernoDamageEvent> inferno;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          damage = self.consumer<DamageEvent>();
          fire = self.consumer<FireDamageEvent>();
          inferno = self.consumer<InfernoDamageEvent>();
        });
    registry.events().send(DamageEvent{1});
    registry.events().send(FireDamageEvent{{2}, 20});
    registry.events().send(InfernoDamageEvent{{{3}, 30}, 300});
    registry.events().send(DamageEvent{4});
    std::vector<int> damages;
    std::vector<int> burns;
    std::size_t infernos = 0;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (const DamageEvent& event : damage.read())
            damages.push_back(event.amount);
          for (const FireDamageEvent& event : fire.read())
            burns.push_back(event.burn);
          infernos = inferno.read().size();
        });
    CHECK((damages == std::vector<int>{1, 2, 3, 4}));
    CHECK((burns == std::vector<int>{20, 30}));
    CHECK(infernos == 1u);
  }

  TEST("a base handler sending a derived type should handle every event, "
       "and the copies at the next update")
  {
    Registry registry;
    EventConsumer<DamageEvent> damage;
    std::vector<int> seen;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          damage = self.consumer<DamageEvent>().onEvent(
              [&](const DamageEvent& event)
              {
                if (event.amount < 10)
                  for (int i = 0; i < 64; ++i)
                    self.send(FireDamageEvent{{event.amount + 10}, 1});
                seen.push_back(event.amount);
              });
        });
    for (int i = 0; i < 3; ++i)
      registry.events().send(DamageEvent{i});
    run(registry, probe);
    CHECK((seen == std::vector<int>{0, 1, 2}));
    run(registry, probe);
    CHECK(seen.size() == 3u + 192u);
    CHECK((seen.size() == 195u && seen[3] == 10 && seen.back() == 12));
  }

  TEST("news with an event base nobody consumes should copy nothing to it")
  {
    Registry registry;
    EventConsumer<FireDamageEvent> fire;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self) { fire = self.consumer<FireDamageEvent>(); });
    registry.events().send(FireDamageEvent{{1}, 2});
    run(registry, probe, [&](Probe&) { fire.read(); });
    std::vector<EventStats> stats;
    registry.events().stats(stats);
    std::size_t baseKept = 1;
    for (const EventStats& type : stats)
      if (type.type.ends_with("::DamageEvent"))
        baseKept = type.kept;
    CHECK(baseKept == 0u);
  }

  TEST("entity news should reach its bases' consumers with its entity, and "
       "readLatest there see the newest of the family")
  {
    Registry registry;
    EventConsumer<HitEvent> hits;
    EventConsumer<HitEvent> latest;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          hits = self.consumer<HitEvent>();
          latest = self.consumer<HitEvent>();
        });
    const Entity a = registry.createEntity();
    registry.events().send(a, HitEvent{1});
    registry.events().send(a, CriticalHitEvent{{2}, 3});
    std::vector<std::pair<Entity, int>> seen;
    std::vector<int> newest;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, hit] : hits.read())
            seen.emplace_back(entity, hit.amount);
          for (auto [entity, hit] : latest.readLatest())
            newest.push_back(hit.amount);
        });
    CHECK((seen == std::vector<std::pair<Entity, int>>{{a, 1}, {a, 2}}));
    CHECK((newest == std::vector<int>{2}));
  }

  TEST("a worker's news should reach its event bases' consumers as it's "
       "taken in")
  {
    Registry registry;
    EventConsumer<DamageEvent> damage;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self) { damage = self.consumer<DamageEvent>(); });
    EventBroadcast<FireDamageEvent> broadcast =
        registry.events().broadcast<FireDamageEvent>();
    std::thread worker([&] { CHECK(broadcast.send(FireDamageEvent{{7}, 1})); });
    worker.join();
    frame(registry);
    std::vector<int> damages;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (const DamageEvent& event : damage.read())
            damages.push_back(event.amount);
        });
    CHECK((damages == std::vector<int>{7}));
  }

  TEST("two requesters asking about one entity should each get only their "
       "own responses, and an observer every one")
  {
    Registry registry;
    EventConsumer<LookRequest> looks;
    EventConsumer<LookFoundResponse> watched;
    EventRequester<LookRequest> first;
    EventRequester<LookRequest> second;
    std::vector<int> firstGot;
    std::vector<int> secondGot;
    std::vector<int> seen;
    Probe& server = registry.addSystem<Probe>(
        [&](Probe& self) { looks = self.consumer<LookRequest>(); });
    Probe& askers = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          first = self.requester<LookRequest>().onResponse<LookFoundResponse>(
              [&](const Entity&, const LookFoundResponse& found)
              { firstGot.push_back(found.value); });
          second = self.requester<LookRequest>().onResponse<LookFoundResponse>(
              [&](const Entity&, const LookFoundResponse& found)
              { secondGot.push_back(found.value); });
          watched = self.consumer<LookFoundResponse>().onEvent(
              [&](const Entity&, const LookFoundResponse& found)
              { seen.push_back(found.value); });
        });
    const Entity a = registry.createEntity();
    first.request(a, LookRequest{1});
    second.request(a, LookRequest{2});
    // A re-request supersedes only its own requester's.
    second.request(a, LookRequest{3});
    CHECK(first.waiting(a));
    CHECK(second.waiting(a));
    std::vector<int> served;
    run(registry,
        server,
        [&](Probe&)
        {
          for (auto [entity, look, responder] : looks.readLatest())
          {
            served.push_back(look.value);
            responder.respond(LookFoundResponse{look.value * 10});
          }
        });
    run(registry, askers);
    CHECK((served == std::vector<int>{1, 3}));
    CHECK((firstGot == std::vector<int>{10}));
    CHECK((secondGot == std::vector<int>{30}));
    CHECK((seen == std::vector<int>{10, 30}));
    CHECK(!first.waiting(a));
    CHECK(!second.waiting(a));
  }

  TEST("a requester's cancel should drop only its own request")
  {
    Registry registry;
    EventConsumer<LookRequest> looks;
    EventRequester<LookRequest> first;
    EventRequester<LookRequest> second;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          looks = self.consumer<LookRequest>();
          first = self.requester<LookRequest>().ignore<LookFoundResponse>();
          second = self.requester<LookRequest>().ignore<LookFoundResponse>();
        });
    const Entity a = registry.createEntity();
    first.request(a, LookRequest{1});
    second.request(a, LookRequest{2});
    first.cancel(a);
    CHECK(!first.waiting(a));
    CHECK(second.waiting(a));
    std::vector<int> current;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, look, responder] : looks.readLatest())
            current.push_back(look.value);
        });
    CHECK((current == std::vector<int>{2}));
  }

  TEST("a destroyed requester should supersede its requests, and a "
       "destroyed entity every requester's")
  {
    Registry registry;
    EventConsumer<LookRequest> looks;
    std::optional<EventRequester<LookRequest>> doomed;
    EventRequester<LookRequest> kept;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          looks = self.consumer<LookRequest>();
          doomed = self.requester<LookRequest>().ignore<LookFoundResponse>();
          kept = self.requester<LookRequest>().ignore<LookFoundResponse>();
        });
    registry.flushEntities();
    const Entity a = registry.createEntity();
    const Entity b = registry.createEntity();
    registry.flushEntities();
    doomed->request(a, LookRequest{1});
    kept.request(a, LookRequest{2});
    std::vector<Responder<LookRequest>> responders;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, look, responder] : looks.read())
            responders.push_back(responder);
        });
    CHECK(responders.size() == 2u);
    doomed.reset();
    CHECK((responders.size() == 2u && responders[0].superseded() &&
           !responders[1].superseded()));
    CHECK((responders.size() == 2u &&
           !responders[0].respond(LookFoundResponse{1})));
    kept.request(b, LookRequest{3});
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, look, responder] : looks.read())
            responders.push_back(responder);
        });
    registry.destroyEntity(a);
    registry.destroyEntity(b);
    frame(registry);
    CHECK((responders.size() == 3u && responders[1].superseded() &&
           responders[2].superseded()));
    CHECK(!kept.waiting(a));
  }

  TEST("a requester should get each response type's responses in send order "
       "per type, the types in the order their handlers were given")
  {
    Registry registry;
    EventConsumer<TravelRequest> travels;
    EventRequester<TravelRequest> traveller;
    std::vector<std::string> seen;
    Probe& server = registry.addSystem<Probe>(
        [&](Probe& self) { travels = self.consumer<TravelRequest>(); });
    Probe& asker = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          traveller =
              self.requester<TravelRequest>()
                  .onResponse<ArrivedResponse>(
                      [&](const Entity&, const ArrivedResponse&)
                      { seen.emplace_back("arrived"); })
                  .onResponse<StepResponse>(
                      [&](const Entity&, const StepResponse& step)
                      { seen.push_back("step" + std::to_string(step.step)); });
        });
    const Entity a = registry.createEntity();
    traveller.request(a, TravelRequest{1});
    run(registry,
        server,
        [&](Probe&)
        {
          for (auto [entity, travel, responder] : travels.read())
          {
            responder.progress(StepResponse{1});
            responder.progress(StepResponse{2});
            responder.respond(ArrivedResponse{1});
          }
        });
    run(registry, asker);
    CHECK((seen == std::vector<std::string>{"arrived", "step1", "step2"}));
  }

  TEST("cancel of entity news with a base should leave the base's copy "
       "current there")
  {
    Registry registry;
    EventConsumer<HitEvent> hits;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self) { hits = self.consumer<HitEvent>(); });
    const Entity a = registry.createEntity();
    registry.events().send(a, CriticalHitEvent{{4}, 2});
    registry.events().cancel<CriticalHitEvent>(a);
    std::vector<int> newest;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, hit] : hits.readLatest())
            newest.push_back(hit.amount);
        });
    CHECK((newest == std::vector<int>{4}));
    registry.events().cancel<HitEvent>(a);
    registry.events().send(a, CriticalHitEvent{{5}, 2});
    registry.events().cancel<HitEvent>(a);
    newest.clear();
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, hit] : hits.readLatest())
            newest.push_back(hit.amount);
        });
    CHECK(newest.empty());
  }

  TEST("entity news about an invalid entity should be dropped, saying so")
  {
    Registry registry;
    LogCapture log;
    registry.events().send(Entity{}, NowhereEvent{1});
    CHECK(log.count(Logger::Level::ERROR, "an invalid entity handle") == 1);
  }

  TEST("a request handler should take its responder by reference, safely "
       "even after destroying the requester")
  {
    Registry registry;
    EventConsumer<LookRequest> looks;
    std::optional<EventRequester<LookRequest>> asker;
    int responded = 0;
    bool supersededAfter = false;
    bool respondedAfter = true;
    Probe& server = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          looks = self.consumer<LookRequest>().onEvent(
              [&](const Entity&,
                  const LookRequest& look,
                  const Responder<LookRequest>& responder)
              {
                if (look.value == 1)
                {
                  responded +=
                      responder.respond(LookFoundResponse{look.value}) ? 1 : 0;
                  return;
                }
                // The asking object goes in the middle of the loop.
                asker.reset();
                supersededAfter = responder.superseded();
                respondedAfter = responder.respond(LookFoundResponse{2});
              });
          asker = self.requester<LookRequest>().ignore<LookFoundResponse>();
        });
    asker->request(registry.createEntity(), LookRequest{1});
    run(registry, server);
    CHECK(responded == 1);
    asker->request(registry.createEntity(), LookRequest{2});
    run(registry, server);
    CHECK(supersededAfter);
    CHECK(!respondedAfter);
  }

  TEST("a requester outliving its events should ask and wait for nothing")
  {
    EventRequester<LookRequest> outlived;
    const Entity a(1u, 0u, 0u);
    {
      Registry registry;
      registry.addSystem<Probe>(
          [&](Probe& self) {
            outlived =
                self.requester<LookRequest>().ignore<LookFoundResponse>();
          });
      outlived.request(registry.createEntity(), LookRequest{1});
    }
    outlived.request(a, LookRequest{2});
    CHECK(!outlived.waiting(a));
    outlived.cancel(a);
    outlived = EventRequester<LookRequest>();
    CHECK(!outlived.valid());
  }

  TEST("a request about an invalid entity should say so, and a cancel "
       "through a stale handle be quiet")
  {
    Registry registry;
    EventRequester<LookRequest> asker;
    registry.addSystem<Probe>(
        [&](Probe& self)
        { asker = self.requester<LookRequest>().ignore<LookFoundResponse>(); });
    LogCapture log;
    asker.request(Entity{}, LookRequest{1});
    CHECK(log.count(Logger::Level::ERROR, "an invalid entity handle") == 1);
    CHECK(log.count(Logger::Level::ERROR, "requests outstanding") == 0);
    registry.flushEntities();
    const Entity gone = registry.createEntity();
    registry.flushEntities();
    asker.request(gone, LookRequest{2});
    registry.destroyEntity(gone);
    frame(registry);
    asker.cancel(gone);
    CHECK(log.count(Logger::Level::WARN, "was dropped") == 0);
  }

  TEST("a held responder should stay superseded when its requester's number "
       "and ticket are reused")
  {
    Registry registry;
    EventConsumer<LookRequest> looks;
    std::optional<EventRequester<LookRequest>> first;
    EventRequester<LookRequest> second;
    std::vector<int> secondGot;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          looks = self.consumer<LookRequest>();
          first = self.requester<LookRequest>().ignore<LookFoundResponse>();
        });
    const Entity a = registry.createEntity();
    first->request(a, LookRequest{1});
    Responder<LookRequest> held;
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, look, responder] : looks.read())
            held = responder;
        });
    first.reset();
    // A new requester takes the freed number, and its request the freed
    // ticket.
    Probe& late = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          second = self.requester<LookRequest>().onResponse<LookFoundResponse>(
              [&](const Entity&, const LookFoundResponse& found)
              { secondGot.push_back(found.value); });
        });
    second.request(a, LookRequest{2});
    CHECK(held.superseded());
    CHECK(!held.respond(LookFoundResponse{1}));
    std::thread worker([&] { CHECK(!held.respond(LookFoundResponse{1})); });
    worker.join();
    run(registry,
        probe,
        [&](Probe&)
        {
          for (auto [entity, look, responder] : looks.read())
            responder.respond(LookFoundResponse{look.value * 10});
        });
    frame(registry);
    run(registry, late);
    CHECK((secondGot == std::vector<int>{20}));
    CHECK(!second.waiting(a));
  }

  TEST("a requester destroyed in its own handler should leave the rest "
       "delivered to others")
  {
    Registry registry;
    EventConsumer<LookRequest> looks;
    std::optional<EventRequester<LookRequest>> doomed;
    EventRequester<LookRequest> kept;
    int doomedGot = 0;
    int keptGot = 0;
    Probe& server = registry.addSystem<Probe>(
        [&](Probe& self) { looks = self.consumer<LookRequest>(); });
    Probe& askers = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          doomed = self.requester<LookRequest>().onResponse<LookFoundResponse>(
              [&](const Entity&, const LookFoundResponse&)
              {
                ++doomedGot;
                doomed.reset();
              });
          kept = self.requester<LookRequest>().onResponse<LookFoundResponse>(
              [&](const Entity&, const LookFoundResponse&) { ++keptGot; });
        });
    for (int i = 0; i < 3; ++i)
    {
      const Entity key = registry.createEntity();
      doomed->request(key, LookRequest{i});
      kept.request(key, LookRequest{i});
    }
    run(registry,
        server,
        [&](Probe&)
        {
          for (auto [entity, look, responder] : looks.read())
            responder.respond(LookFoundResponse{look.value});
        });
    run(registry, askers);
    CHECK(doomedGot == 1);
    CHECK(keptGot == 3);
  }

  TEST("responses surviving a cap drop behind a kept range should still "
       "reach their requester")
  {
    Registry registry;
    registry.events().setMaxKept(100);
    EventConsumer<TravelRequest> travels;
    EventRequester<TravelRequest> holder;
    EventRequester<TravelRequest> other;
    std::optional<ResponseRange<TravelRequest, StepResponse>> keptRange;
    int otherSteps = 0;
    Probe& server = registry.addSystem<Probe>(
        [&](Probe& self) { travels = self.consumer<TravelRequest>(); });
    Probe& askers = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          holder = self.requester<TravelRequest>()
                       .onResponses<StepResponse>(
                           [&](ResponseRange<TravelRequest, StepResponse> steps)
                           { keptRange.emplace(steps); })
                       .ignore<ArrivedResponse>();
          other =
              self.requester<TravelRequest>()
                  .onResponse<StepResponse>(
                      [&](const Entity&, const StepResponse&) { ++otherSteps; })
                  .ignore<ArrivedResponse>();
        });
    const Entity a = registry.createEntity();
    holder.request(a, TravelRequest{1});
    other.request(a, TravelRequest{2});
    std::vector<Responder<TravelRequest>> responders;
    run(registry,
        server,
        [&](Probe&)
        {
          for (auto [entity, travel, responder] : travels.read())
            responders.push_back(responder);
        });
    // The holder's handler keeps its range: later steps wait behind it.
    responders[0].progress(StepResponse{0});
    run(registry, askers);
    LogCapture log;
    for (int i = 0; i < 100; ++i)
    {
      responders[0].progress(StepResponse{i});
      responders[1].progress(StepResponse{i});
    }
    frame(registry);
    CHECK(log.count(Logger::Level::ERROR, "behind a range kept past") == 1);
    keptRange.reset();
    run(registry, askers);
    // The cap kept the newest, half of them the other requester's.
    CHECK(otherSteps > 0);
  }

  TEST("a requester whose owner never runs should hold no more of its "
       "responses than the cap")
  {
    Registry registry;
    registry.events().setMaxKept(1000);
    EventConsumer<TravelRequest> travels;
    EventRequester<TravelRequest> idle;
    Probe& server = registry.addSystem<Probe>(
        [&](Probe& self) { travels = self.consumer<TravelRequest>(); });
    Probe& asker = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          idle = self.requester<TravelRequest>()
                     .onResponse<StepResponse>(
                         [](const Entity&, const StepResponse&) {})
                     .ignore<ArrivedResponse>();
        });
    asker.setEnabled(false);
    idle.request(registry.createEntity(), TravelRequest{1});
    Responder<TravelRequest> streaming;
    run(registry,
        server,
        [&](Probe&)
        {
          for (auto [entity, travel, responder] : travels.read())
            streaming = responder;
        });
    LogCapture log;
    for (int frame = 0; frame < 70; ++frame)
    {
      for (int i = 0; i < 1000; ++i)
        streaming.progress(StepResponse{i});
      registry.events().update();
    }
    CHECK(log.count(Logger::Level::WARN, "responses it hasn't read") == 0);
  }

  TEST("stats should name each type's kind")
  {
    Registry registry;
    EventConsumer<NewsEvent> news;
    EventConsumer<MovedEvent> moved;
    EventConsumer<RouteRequest> route;
    EventRequester<RouteRequest> asker;
    Probe& probe = registry.addSystem<Probe>(
        [&](Probe& self)
        {
          news = self.consumer<NewsEvent>();
          moved = self.consumer<MovedEvent>();
          route = self.consumer<RouteRequest>();
          asker = self.requester<RouteRequest>()
                      .ignore<RouteLegResponse>()
                      .ignore<RouteDoneResponse>();
        });
    const Entity a = registry.createEntity();
    registry.events().send(NewsEvent{1});
    registry.events().send(a, MovedEvent{1});
    asker.request(a, RouteRequest{1});
    run(registry,
        probe,
        [&](Probe&)
        {
          news.read();
          moved.readLatest();
          route.read();
        });
    std::vector<EventStats> stats;
    registry.events().stats(stats);
    const auto find = [&](std::string_view name) -> const EventStats*
    {
      for (const EventStats& type : stats)
        if (type.type.ends_with(name))
          return &type;
      return nullptr;
    };
    const EventStats* newsStats = find("::NewsEvent");
    const EventStats* movedStats = find("::MovedEvent");
    const EventStats* routeStats = find("::RouteRequest");
    CHECK((newsStats && newsStats->kind == "news"));
    CHECK((movedStats && movedStats->kind == "entity news"));
    CHECK((routeStats && routeStats->kind == "request" &&
           routeStats->consumers == 1u));
  }

  TEST("logEvents should log keyed sends with their key and number")
  {
    debugFeatures::setLogEvents("LoggedKeyedEvent");
    LogCapture log;
    Registry registry;
    const Entity a = registry.createEntity();
    registry.events().send(a, LoggedKeyedEvent{1});
    registry.events().cancel<LoggedKeyedEvent>(a);
    debugFeatures::setLogEvents("");
    if (debugFeatures::enabled())
    {
      CHECK(log.count(Logger::Level::INFO, "[entity news] #0 for entity") == 1);
      CHECK(log.count(Logger::Level::INFO, "cancelled") == 1);
    }
  }

  return testing::report("eventKindsTests");
}
