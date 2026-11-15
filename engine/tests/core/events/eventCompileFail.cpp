// Uses of events that the types' declarations forbid: each case must fail to
// compile with its static_assert's message, or the name of the template that
// fails, which names the types (CMake builds one target per case and checks
// the output). Case 0 holds the allowed uses and must compile.

#include <engine/core/ecs/ecs.h>
#include <engine/core/events/events.h>
#include <engine/core/events/responder.h>

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

struct [[sfs::event_request(FoundResponse, FailedResponse)]] SearchRequest
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

// Not marked: not an event.
struct Plain
{
  int value = 0;
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
           sfs::Responder<SearchRequest> responder)
        {
          responder.progress(FoundResponse{});
          responder.respond(FailedResponse{});
        });
    m_damage = consumer<DamageEvent>();
    m_search = requester<SearchRequest>()
                   .onResponse<FoundResponse>(
                       [](const sfs::Entity&, const FoundResponse&){})
                   .ignore<FailedResponse>();
    m_both = requester<SearchRequest>()
                 .onResponses<FailedResponse>(
                     [](sfs::ResponseRange<SearchRequest, FailedResponse>){})
                 .onResponse<FoundResponse>(
                     [](const sfs::Entity&, const FoundResponse&){});
#elif SFS_COMPILE_FAIL_CASE == 1
    m_search = requester<SearchRequest>().onResponse<FoundResponse>(
        [](const sfs::Entity&, const FoundResponse&){});
#elif SFS_COMPILE_FAIL_CASE == 2
    m_search =
        requester<SearchRequest>()
            .onResponse<NewsEvent>([](const sfs::Entity&, const NewsEvent&){})
            .ignore<FoundResponse>()
            .ignore<FailedResponse>();
#elif SFS_COMPILE_FAIL_CASE == 3
    m_search = requester<SearchRequest>()
                   .ignore<FoundResponse>()
                   .ignore<FailedResponse>()
                   .ignore<FoundResponse>();
#elif SFS_COMPILE_FAIL_CASE == 4
    m_keyed = consumer<KeyedEvent>().onEvent([](const KeyedEvent&){});
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
#elif SFS_COMPILE_FAIL_CASE == 5
    sfs::Responder<SearchRequest> responder;
    responder.respond(NewsEvent{});
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
#endif
  }

private:
  sfs::EventConsumer<NewsEvent> m_news;
  sfs::EventConsumer<KeyedEvent> m_keyed;
  sfs::EventConsumer<FoundResponse> m_found;
  sfs::EventConsumer<SearchRequest> m_searches;
  sfs::EventConsumer<DamageEvent> m_damage;
  sfs::EventRequester<SearchRequest> m_search;
  sfs::EventRequester<SearchRequest> m_both;
};

} // namespace

// Instantiated so its members are compiled.
void useCompileFailCase()
{
  sfs::Registry registry;
  registry.addSystem<Uses>();
}
