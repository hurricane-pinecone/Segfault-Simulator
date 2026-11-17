// Uses of the ECS that its API forbids: each case must fail to compile with
// its static_assert's message, or the error naming what's wrong (CMake builds
// one target per case and checks the output). Case 0 holds the allowed uses
// and must compile.

#include "engine/core/ecs/ecs.h"
#include "engine/core/events/events.h"
#include "engine/core/events/responder.h"

#ifndef SFS_COMPILE_FAIL_CASE
  #define SFS_COMPILE_FAIL_CASE 0
#endif

namespace
{

struct Health
{
  int value = 0;
};

struct Hidden
{
};

struct [[sfs::scene_data]] Score
{
  int points = 0;
};

struct [[sfs::event_news]] HitEvent
{
  int value = 0;
};

struct [[sfs::event_entity_news]] DiedEvent
{
  int value = 0;
};

struct [[sfs::event_response]] FoundResponse
{
  int value = 0;
};

struct [[sfs::event_request(FoundResponse)]] SearchRequest
{
  int value = 0;
};

[[maybe_unused]] void entityUses(sfs::Registry& registry)
{
  const sfs::Entity entity = registry.createEntity();
#if SFS_COMPILE_FAIL_CASE == 0
  // A handle's constness is the handle's: a const one adds, reads and removes.
  entity.addComponent<Health>(Health{3}).addTag<Hidden>();
  if (entity.hasComponent<Health>() && entity.getComponent<Health>())
    entity.removeComponent<Hidden>();
#elif SFS_COMPILE_FAIL_CASE == 10
  // An entity doesn't lead to its registry.
  (void)entity.registryOf();
#elif SFS_COMPILE_FAIL_CASE == 11
  // Nor can it be pointed at another.
  sfs::Entity copy = entity;
  copy.setRegistry(&registry);
#endif
}

class Watcher : public sfs::Service
{
protected:
  void create() override
  {
#if SFS_COMPILE_FAIL_CASE == 0
    send(HitEvent{1});
    m_hits = consumer<HitEvent>().onEvent(
        [this](const HitEvent& hit)
        {
          const sfs::Entity made = createEntity();
          made.addComponent<Health>(Health{hit.value});
          broadcast<HitEvent>();

          if (Score* own = sceneData<Score>())
            own->points += hit.value;
        });
    m_deaths = consumer<DiedEvent>()
                   .onLatest(
                       [this](const sfs::Entity& entity, const DiedEvent&)
                       {
                         if (!entity)
                           return;

                         entity.addComponent<Health>().addTag<Hidden>();

                         if (entity.hasComponent<Health>() &&
                             entity.getComponent<Health>())
                           entity.removeComponent<Hidden>();

                         send(entity, DiedEvent{1});
                         cancel<DiedEvent>(entity);
                         destroyEntity(entity);
                       })
                   .onEvents([](sfs::EventRange<DiedEvent>){});
    m_latest = consumer<DiedEvent>().onLatestEvents(
        [](sfs::LatestEventRange<DiedEvent>){});
    m_searches = consumer<SearchRequest>().onEvent(
        [this](const sfs::Entity& entity,
               const SearchRequest&,
               const sfs::Responder<SearchRequest>& responder)
        {
          if (entity.isValid())
            responder.respond(FoundResponse{1});
          else
            m_searches.respond(entity, FoundResponse{2});
        });
    m_asks = requester<SearchRequest>().onResponse(
        [](const sfs::Entity&, const FoundResponse&){});
#elif SFS_COMPILE_FAIL_CASE == 1
    query<Health>();
#elif SFS_COMPILE_FAIL_CASE == 2
    getEntities();
#elif SFS_COMPILE_FAIL_CASE == 3
    registerComponent<Health>();
#elif SFS_COMPILE_FAIL_CASE == 4
    observe<Health>([](const sfs::Entity&){});
#elif SFS_COMPILE_FAIL_CASE == 5
    registry->query<Health>();
#elif SFS_COMPILE_FAIL_CASE == 6
    m_hits = consumer<HitEvent>();
#elif SFS_COMPILE_FAIL_CASE == 13
    // A component isn't scene data.
    (void)sceneData<Health>();
#endif
  }

#if SFS_COMPILE_FAIL_CASE == 7
  void update(double) override {}
#endif

private:
  sfs::EventConsumer<HitEvent> m_hits;
  sfs::EventConsumer<DiedEvent> m_deaths;
  sfs::EventConsumer<DiedEvent> m_latest;
  sfs::EventConsumer<SearchRequest> m_searches;
  sfs::EventRequester<SearchRequest> m_asks;
};

// A service with no create(): it would make no handles and never run.
class [[maybe_unused]] NoCreate : public sfs::Service
{
};

class SomeSystem : public sfs::System
{
};

[[maybe_unused]] void hosting(sfs::Registry& registry)
{
  registry.addService<Watcher>();
#if SFS_COMPILE_FAIL_CASE == 0
  registry.addSceneData<Score>(Score{1});
  if (const Score* score = registry.sceneData<const Score>())
    registry.sceneData<Score>()->points = score->points + 1;
  registry.removeSceneData<Score>();
#elif SFS_COMPILE_FAIL_CASE == 12
  (void)registry.sceneData<const Health>();
#elif SFS_COMPILE_FAIL_CASE == 14
  registry.addSceneData<Health>();
#elif SFS_COMPILE_FAIL_CASE == 15
  // Scene data isn't a component: not on an entity, nor in a query.
  registry.createEntity().addComponent<Score>();
#elif SFS_COMPILE_FAIL_CASE == 16
  (void)registry.query<Score>();
#elif SFS_COMPILE_FAIL_CASE == 17
  (void)registry.createEntity().getComponent<Score>();
#elif SFS_COMPILE_FAIL_CASE == 18
  // A reader takes it const, and can't write through it.
  registry.sceneData<const Score>()->points = 1;
#elif SFS_COMPILE_FAIL_CASE == 8
  registry.addService<NoCreate>();
#elif SFS_COMPILE_FAIL_CASE == 9
  registry.addService<SomeSystem>();
#endif
}

} // namespace
