// Uses of events that the types' declarations forbid: each case must fail to
// compile with its static_assert's message, or the name of the template that
// fails, which names the types (CMake builds one target per case and checks
// the output). Case 0 holds the allowed uses and must compile.

#include "engine/core/ecs/ecs.h"
#include "engine/core/events/events.h"
#include "engine/core/events/responder.h"

#include <memory>

#ifndef SFS_COMPILE_FAIL_CASE
  #define SFS_COMPILE_FAIL_CASE 0
#endif

namespace
{

struct [[sfs::event_news]] NewsEvent
{
  int value = 0;
};

struct [[sfs::event_entity_news]] KeyedEvent
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

struct [[sfs::event_response]] StepResponse
{
  int value = 0;
};

struct [[sfs::event_request(FoundResponse,
                            progress = StepResponse,
                            failure = FailedResponse)]] SearchRequest
{
  int value = 0;
};

struct [[sfs::event_response]] OpenedResponse
{
  int value = 0;
};

// Only a success.
struct [[sfs::event_request(OpenedResponse)]] DoorRequest
{
  int value = 0;
};

struct [[sfs::event_news]] DamageEvent
{
  float amount = 0.0f;
};

struct [[sfs::event_news]] FireDamageEvent : DamageEvent
{
  float burnSeconds = 0.0f;
};

// A response that can only be moved.
struct [[sfs::event_response]] MovedResponse
{
  std::unique_ptr<int> value;
};

struct [[sfs::event_request(MovedResponse)]] MoveRequest
{
  int value = 0;
};

// Not marked: not an event.
struct Plain
{
  int value = 0;
};

// Not an owner of event handles: its member functions can't be handlers.
struct Unrelated
{
  void onNews(const NewsEvent&) {}
};

class Uses : public sfs::System
{
protected:
  void create() override
  {
#if SFS_COMPILE_FAIL_CASE == 0
    m_news = consumer<NewsEvent>();
    m_keyed = consumer<KeyedEvent>().onLatest(
        [](const sfs::Entity&, const KeyedEvent&){});
    m_found = consumer<FoundResponse>().onEvent(
        [](const sfs::Entity&, const FoundResponse&){});
    m_searches = consumer<SearchRequest>().onEvent(
        [](const sfs::Entity&,
           const SearchRequest&,
           const sfs::Responder<SearchRequest>& responder)
        {
          responder.progress(StepResponse{});

          if (responder.superseded())
            responder.fail(FailedResponse{});
          else
            responder.respond(FoundResponse{});
        });
    m_damage = consumer<DamageEvent>();
    m_search = requester<SearchRequest>()
                   .onResponse([](const sfs::Entity&, const FoundResponse&){})
                   .onProgress([](const sfs::Entity&, const StepResponse&){})
                   .onFailure([](const sfs::Entity&, const FailedResponse&){});
    m_both = requester<SearchRequest>().ignoreFailure().onResponses(
        [](sfs::ResponseRange<SearchRequest, FoundResponse>){});
    m_door = requester<DoorRequest>().onResponse(
        [](const sfs::Entity&, const OpenedResponse&){});
    // Member functions of the owner, const, noexcept or neither.
    m_news =
        consumer<NewsEvent>().onEvent(&Uses::onNews).onEvents(&Uses::onAllNews);
    m_keyed = consumer<KeyedEvent>()
                  .onEvent(&Uses::onKeyed)
                  .onLatest(&Uses::onKeyed)
                  .onLatestEvents(&Uses::onNewestKeyed);
    m_searches = consumer<SearchRequest>().onEvent(&Uses::onSearch);
    m_search = requester<SearchRequest>()
                   .onResponse(&Uses::onFound)
                   .onProgress(&Uses::onStep)
                   .onFailure(&Uses::onFailed);
    m_both = requester<SearchRequest>()
                 .onResponses(&Uses::onFounds)
                 .onProgresses(&Uses::onSteps)
                 .onFailures(&Uses::onFailures);
#elif SFS_COMPILE_FAIL_CASE == 1
    m_search = requester<SearchRequest>().onFailure(
        [](const sfs::Entity&, const FailedResponse&){});
#elif SFS_COMPILE_FAIL_CASE == 2
    m_search = requester<SearchRequest>().onResponse(
        [](const sfs::Entity&, const FoundResponse&){});
#elif SFS_COMPILE_FAIL_CASE == 3
    m_search = requester<SearchRequest>()
                   .onResponse([](const sfs::Entity&, const FoundResponse&){})
                   .ignoreFailure()
                   .onResponse([](const sfs::Entity&, const FoundResponse&){});
#elif SFS_COMPILE_FAIL_CASE == 4
    m_keyed = consumer<KeyedEvent>().onEvent([](const KeyedEvent&){});
#elif SFS_COMPILE_FAIL_CASE == 20
    m_door = requester<DoorRequest>()
                 .onResponse([](const sfs::Entity&, const OpenedResponse&){})
                 .onProgress([](const sfs::Entity&, const OpenedResponse&){});
#elif SFS_COMPILE_FAIL_CASE == 21
    m_door = requester<DoorRequest>()
                 .onResponse([](const sfs::Entity&, const OpenedResponse&){})
                 .ignoreFailure();
#elif SFS_COMPILE_FAIL_CASE == 22
    m_search = requester<SearchRequest>()
                   .onResponse([](const sfs::Entity&, const FailedResponse&){})
                   .ignoreFailure();
#elif SFS_COMPILE_FAIL_CASE == 27
    auto news = requester<NewsEvent>();
#elif SFS_COMPILE_FAIL_CASE == 29
    m_news = consumer<NewsEvent>().onEvent(&Unrelated::onNews);
#elif SFS_COMPILE_FAIL_CASE == 30
    m_keyed = consumer<KeyedEvent>().onEvent(&Uses::onNews);
#elif SFS_COMPILE_FAIL_CASE == 31
    m_news = consumer<NewsEvent>().onEvent(&Uses::m_count);
#elif SFS_COMPILE_FAIL_CASE == 32
    m_door = requester<DoorRequest>().onResponse(&Uses::onFound);
#endif
  }

  void update(double) override
  {
    const sfs::Entity key;
#if SFS_COMPILE_FAIL_CASE == 0
    send(NewsEvent{});
    send(key, KeyedEvent{});
    send(FireDamageEvent{{5.0f}, 3.0f});
    m_search.request(key, SearchRequest{});
    m_search.cancel(key);
    (void)m_search.waiting(key);
    for (auto [entity, event] : m_keyed.readLatest())
      (void)event;
    sfs::EventBroadcast<NewsEvent> news = broadcast<NewsEvent>();
    sfs::EventBroadcast<KeyedEvent> keyed = broadcast<KeyedEvent>();
    news.send(NewsEvent{});
    keyed.send(key, KeyedEvent{});
    // A server answering later, by entity.
    if (m_searches.open(key))
    {
      m_searches.progress(key, StepResponse{});
      m_searches.respond(key, FoundResponse{});
      m_searches.fail(key, FailedResponse{});
    }
#elif SFS_COMPILE_FAIL_CASE == 5
    sfs::Responder<SearchRequest> responder;
    responder.respond(FailedResponse{});
#elif SFS_COMPILE_FAIL_CASE == 6
    send(key, FoundResponse{});
#elif SFS_COMPILE_FAIL_CASE == 7
    sfs::EventBroadcast<FoundResponse> found = broadcast<FoundResponse>();
#elif SFS_COMPILE_FAIL_CASE == 8
    send(Plain{});
#elif SFS_COMPILE_FAIL_CASE == 9
    send(KeyedEvent{});
#elif SFS_COMPILE_FAIL_CASE == 10
    send(key, NewsEvent{});
#elif SFS_COMPILE_FAIL_CASE == 11
    send(key, SearchRequest{});
#elif SFS_COMPILE_FAIL_CASE == 12
    for (const NewsEvent& news : m_news.readLatest())
      (void)news;
#elif SFS_COMPILE_FAIL_CASE == 13
    sfs::Responder<NewsEvent> responder;
#elif SFS_COMPILE_FAIL_CASE == 14
    sfs::EventBroadcast<SearchRequest> requests;
#elif SFS_COMPILE_FAIL_CASE == 15
    sfs::EventRequester<NewsEvent> requester;
#elif SFS_COMPILE_FAIL_CASE == 16
    sfs::Events events;
    events.cancel<NewsEvent>(key);
#elif SFS_COMPILE_FAIL_CASE == 17
    sfs::EventConsumer<Plain> plain;
#elif SFS_COMPILE_FAIL_CASE == 18
    m_news.respond(key, FoundResponse{});
#elif SFS_COMPILE_FAIL_CASE == 19
    m_searches.respond(key, NewsEvent{});
#elif SFS_COMPILE_FAIL_CASE == 23
    sfs::Responder<DoorRequest> responder;
    responder.fail(OpenedResponse{});
#elif SFS_COMPILE_FAIL_CASE == 24
    sfs::Responder<SearchRequest> responder;
    responder.progress(FoundResponse{});
#elif SFS_COMPILE_FAIL_CASE == 25
    m_searches.fail(key, StepResponse{});
#elif SFS_COMPILE_FAIL_CASE == 26
    sfs::Responder<DoorRequest> responder;
    responder.progress(OpenedResponse{});
#elif SFS_COMPILE_FAIL_CASE == 28
    sfs::EventConsumer<MoveRequest> moves;
    moves.respond(key, MovedResponse{});
#endif
  }

private:
  void onAllNews(sfs::EventRange<NewsEvent>) {}
  void onFailed(const sfs::Entity&, const FailedResponse&) {}
  void onFailures(sfs::ResponseRange<SearchRequest, FailedResponse>) const {}
  void onFound(const sfs::Entity&, const FoundResponse&) noexcept {}
  void onFounds(sfs::ResponseRange<SearchRequest, FoundResponse>) {}
  void onKeyed(const sfs::Entity&, const KeyedEvent&) const {}
  void onNewestKeyed(sfs::LatestEventRange<KeyedEvent>) {}
  void onNews(const NewsEvent&) {}
  void onSearch(const sfs::Entity&,
                const SearchRequest&,
                const sfs::Responder<SearchRequest>&) const noexcept
  {
  }
  void onStep(const sfs::Entity&, const StepResponse&) const {}
  void onSteps(sfs::ResponseRange<SearchRequest, StepResponse>) {}

  int m_count = 0;
  sfs::EventConsumer<NewsEvent> m_news;
  sfs::EventConsumer<KeyedEvent> m_keyed;
  sfs::EventConsumer<FoundResponse> m_found;
  sfs::EventConsumer<SearchRequest> m_searches;
  sfs::EventConsumer<DamageEvent> m_damage;
  sfs::EventRequester<SearchRequest> m_search;
  sfs::EventRequester<SearchRequest> m_both;
  sfs::EventRequester<DoorRequest> m_door;
};

} // namespace

// Instantiated so its members are compiled.
void useCompileFailCase()
{
  sfs::Registry registry;
  registry.addSystem<Uses>();
}
