#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>

#include <chrono>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

// The ECS's benchmark: what one component add or remove on a flushed entity
// costs with a game's worth of cached query lists and systems (120 lists over
// 40 component types, 60 systems, 2000 entities). It times toggling a
// component that no list names, one that a few name, and one that many name,
// and prints ns per change. It checks only that the lists stay right.

using namespace sfs;

namespace
{

constexpr int kTypes = 40;
constexpr int kLists = 120;
constexpr int kSystems = 60;
constexpr int kEntities = 2000;
// Named by about 40% of the lists, as a transform is.
constexpr int kCommon = 0;
// Named by a few lists.
constexpr int kRare = 36;
// Named by no list.
constexpr int kUnnamed = kTypes;

template <int N>
struct Part
{
};

// List I: a common part in 40% of them, two others spread over the types,
// and an Excluding group in every fourth.
template <int I>
void askForList(Registry& reg, std::set<const std::vector<Entity>*>& lists)
{
  constexpr int n = kTypes - 4;
  constexpr int a = 1 + I % n;
  constexpr int b = 1 + (I / n * 7 + I * 13 + 5) % n;
  constexpr int c = 1 + (I / n + I * 3 + 11) % n;
  if constexpr (I % 5 < 2 && I % 4 == 0)
    lists.insert(&reg.query<Part<kCommon>, Part<a>, Excluding<Part<b>>>());
  else if constexpr (I % 5 < 2)
    lists.insert(&reg.query<Part<kCommon>, Part<a>>());
  else if constexpr (I % 4 == 0)
    lists.insert(&reg.query<Part<a>, Any<Part<b>, Part<c>>>());
  else
    lists.insert(&reg.query<Part<a>, Part<b>>());
}

template <int... Is>
void askForLists(Registry& reg,
                 std::set<const std::vector<Entity>*>& lists,
                 std::integer_sequence<int, Is...>)
{
  (askForList<Is>(reg, lists), ...);
}

// A system per N, working on two parts.
template <int N>
class BenchSystem : public System
{
protected:
  void create() override
  {
    registerComponent<Part<(N % 2 == 0) ? kCommon : 1 + N % (kTypes - 4)>>();
    registerComponent<Part<1 + (N * 11) % (kTypes - 4)>>();
  }
  void onEntityAdded(const Entity&) override { ++joined; }
  void onEntityRemoved(const Entity&) override { ++left; }

public:
  int joined = 0;
  int left = 0;
};

template <int... Ns>
void addSystems(Registry& reg, std::integer_sequence<int, Ns...>)
{
  (reg.addSystem<BenchSystem<Ns>>(), ...);
}

// Gives entity `i` part N one time in three, never kRare.
template <int N>
void maybeGivePart(Entity& entity, int i)
{
  if (N != kRare && (i * 31 + N * 17) % 3 == 0)
    entity.addComponent<Part<N>>();
}

// Gives entity `i` kCommon and about a third of the other parts.
template <int... Ns>
void giveParts(Entity& entity, int i, std::integer_sequence<int, Ns...>)
{
  (maybeGivePart<Ns>(entity, i), ...);
  entity.addComponent<Part<kCommon>>();
}

// Toggles TPart on and off across the entities: ns per change.
template <typename TPart>
double timeToggles(std::vector<Entity>& entities, int rounds)
{
  const auto start = std::chrono::steady_clock::now();
  for (int r = 0; r < rounds; ++r)
    for (Entity& entity : entities)
    {
      if (entity.hasComponent<TPart>())
        entity.removeComponent<TPart>();
      else
        entity.addComponent<TPart>();
    }
  const auto end = std::chrono::steady_clock::now();
  const double changes = static_cast<double>(rounds) * entities.size();
  return std::chrono::duration<double, std::nano>(end - start).count() /
         changes;
}

} // namespace

int main()
{
  TEST("one component change should cost little with many lists and systems")
  {
    Registry reg;
    std::set<const std::vector<Entity>*> lists;
    askForLists(reg, lists, std::make_integer_sequence<int, kLists>());
    addSystems(reg, std::make_integer_sequence<int, kSystems>());
    std::vector<Entity> entities;
    entities.reserve(kEntities);
    for (int i = 0; i < kEntities; ++i)
    {
      entities.push_back(reg.createEntity());
      giveParts(entities.back(), i, std::make_integer_sequence<int, kTypes>());
    }
    reg.flushEntities();

    constexpr int rounds = 20;
    // Warm up, then time each kind.
    timeToggles<Part<kUnnamed>>(entities, 2);
    const double unnamed = timeToggles<Part<kUnnamed>>(entities, rounds);
    const double rare = timeToggles<Part<kRare>>(entities, rounds);
    const double common = timeToggles<Part<kCommon>>(entities, rounds);
    std::printf("  %zu lists, %d systems, %d entities: ns per change: "
                "named by none %.0f, by a few %.0f, by many %.0f\n",
                lists.size(),
                kSystems,
                kEntities,
                unnamed,
                rare,
                common);

    // Every entity has kCommon again (an even number of toggles).
    CHECK((reg.query<Part<kCommon>>().size() == kEntities));
    CHECK((reg.query<Part<kUnnamed>>().empty()));
    CHECK((reg.query<Part<kRare>>().empty()));
  }

  return testing::report("ecsBenchTests");
}
