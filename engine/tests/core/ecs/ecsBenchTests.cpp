#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The ECS's benchmarks, meant for a Release build (the Debug and Static trees
// are unoptimised). Each prints `bench <case> <entities> <ns>` lines.
//
// Lookups and loops: 2,000 and 50,000 entities, A on all, B on half and C on a
// third, and a quarter as many stale handles (their ids reused); handles are
// visited in random orders from a fixed seed. Each case is the best of 200
// rounds (a round going through 2,000 entities 25 times), its result kept
// through a volatile sink, in ns per row or lookup.
//
// Changes: what one component add or remove on a flushed entity costs with a
// game's worth of cached query lists and systems (120 lists over 40 component
// types, 60 systems, 2000 entities), toggling a component that no list names,
// one that a few name, and one that many name, in ns per change.
//
// Every case is run in three passes, each on a registry made afresh, and the
// best pass is printed at the end.
//
// With SFS_BENCH_BASELINE set to a file of an earlier run's output, it checks
// the speed gates against it (docs/core/ecs/index.md).

using namespace sfs;

namespace
{

volatile uint64_t g_sink = 0;

constexpr int kPasses = 3;

// One result per case and entity count: the best pass's.
using Results = std::map<std::pair<std::string, int>, double>;
Results g_results;

void record(const std::string& name, int entities, double ns)
{
  const auto [at, made] = g_results.try_emplace({name, entities}, ns);
  if (!made)
    at->second = std::min(at->second, ns);
}

void printResults()
{
  for (const auto& [key, ns] : g_results)
    std::printf("bench %s %d %.3f\n", key.first.c_str(), key.second, ns);
}

// The best time of `rounds` rounds, each running `body` (which returns a
// checksum) `repeats` times, in ns per one of `rows` (a run's).
template <typename TBody>
double bestNs(int rounds, int repeats, std::size_t rows, TBody&& body)
{
  double best = std::numeric_limits<double>::max();
  for (int r = 0; r < rounds; ++r)
  {
    uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < repeats; ++i)
      sum += body();
    const auto end = std::chrono::steady_clock::now();
    g_sink = g_sink + sum;
    best = std::min(
        best, std::chrono::duration<double, std::nano>(end - start).count());
  }
  return rows ? best / (static_cast<double>(rows) * repeats) : 0.0;
}

struct BenchA
{
  uint32_t value = 1;
  float pad[3] = {};
};
struct BenchB
{
  uint32_t value = 2;
  float pad[3] = {};
};
struct BenchC
{
  uint32_t value = 3;
  float pad[3] = {};
};

// A registry of `count` entities as the header describes, and the handle
// lists the cases visit.
struct World
{
  Registry reg;
  std::vector<Entity> withB;    // living, with B, shuffled
  std::vector<Entity> withoutB; // living, without B, shuffled
  std::vector<Entity> mixed;    // living and stale, shuffled

  explicit World(int count)
  {
    std::mt19937 random(12345u + static_cast<unsigned>(count));
    std::vector<Entity> made;
    made.reserve(count);
    for (int i = 0; i < count; ++i)
      made.push_back(reg.createEntity());
    reg.flushEntities();

    std::vector<Entity> stale = made;
    std::shuffle(stale.begin(), stale.end(), random);
    stale.resize(count / 4);
    for (const Entity& entity : stale)
      reg.destroyEntity(entity);
    reg.flushEntities();
    for (int i = 0; i < count / 4; ++i)
      reg.createEntity();
    reg.flushEntities();

    const std::vector<Entity> living = reg.entities();
    for (Entity entity : living)
    {
      const Entity::EntityId id = entity.getId();
      entity.addComponent<BenchA>();
      if (id % 2 == 0)
        entity.addComponent<BenchB>();
      if (id % 3 == 0)
        entity.addComponent<BenchC>();
      (id % 2 == 0 ? withB : withoutB).push_back(entity);
    }
    reg.flushEntities();

    mixed = living;
    mixed.insert(mixed.end(), stale.begin(), stale.end());
    std::shuffle(withB.begin(), withB.end(), random);
    std::shuffle(withoutB.begin(), withoutB.end(), random);
    std::shuffle(mixed.begin(), mixed.end(), random);
  }
};

constexpr int kRounds = 200;

void benchLookups(int count)
{
  World world(count);
  Registry& reg = world.reg;
  // Small registries are gone through several times a round, so a round is
  // long enough to time well.
  const int repeats = std::max(1, 50000 / count);

  const std::vector<Entity>& two = reg.query<BenchA, BenchB>();
  const std::vector<Entity>& three = reg.query<BenchA, BenchB, BenchC>();
  const QueryView<BenchA, BenchB> twoView = reg.query<BenchA, BenchB>();
  const QueryView<BenchA, BenchB, BenchC> threeView =
      reg.query<BenchA, BenchB, BenchC>();

  record("entity-loop-vector",
         count,
         bestNs(kRounds,
                repeats,
                two.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : two)
                    sum += entity.getId();
                  return sum;
                }));
  record("entity-loop-view",
         count,
         bestNs(kRounds,
                repeats,
                twoView.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : twoView)
                    sum += entity.getId();
                  return sum;
                }));

  record("rows-2",
         count,
         bestNs(kRounds,
                repeats,
                twoView.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (auto [entity, a, b] : twoView)
                    sum += a.value + b.value;
                  return sum;
                }));
  record("rows-3",
         count,
         bestNs(kRounds,
                repeats,
                threeView.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (auto [entity, a, b, c] : threeView)
                    sum += a.value + b.value + c.value;
                  return sum;
                }));

  // The same rows without the entity: one component by its conversion,
  // several through components(). Each should cost what its entity row does.
  const QueryView<BenchA> oneView = reg.query<BenchA>();
  record("rows-1",
         count,
         bestNs(kRounds,
                repeats,
                oneView.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (auto [entity, a] : oneView)
                    sum += a.value;
                  return sum;
                }));
  record("component-1",
         count,
         bestNs(kRounds,
                repeats,
                oneView.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const BenchA& a : oneView)
                    sum += a.value;
                  return sum;
                }));
  record("components-2",
         count,
         bestNs(kRounds,
                repeats,
                twoView.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (auto [a, b] : twoView.components())
                    sum += a.value + b.value;
                  return sum;
                }));
  record("components-3",
         count,
         bestNs(kRounds,
                repeats,
                threeView.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (auto [a, b, c] : threeView.components())
                    sum += a.value + b.value + c.value;
                  return sum;
                }));

  record("getComponent-loop-2",
         count,
         bestNs(kRounds,
                repeats,
                two.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : two)
                    sum += entity.getComponent<BenchA>()->value +
                           entity.getComponent<BenchB>()->value;
                  return sum;
                }));
  record("finder-loop-2",
         count,
         bestNs(kRounds,
                repeats,
                two.size(),
                [&]
                {
                  uint64_t sum = 0;
                  const auto a = reg.finder<BenchA>();
                  const auto b = reg.finder<BenchB>();
                  for (const Entity& entity : two)
                    sum += a(entity.getId())->value + b(entity.getId())->value;
                  return sum;
                }));

  record("getComponent-loop-3",
         count,
         bestNs(kRounds,
                repeats,
                three.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : three)
                    sum += entity.getComponent<BenchA>()->value +
                           entity.getComponent<BenchB>()->value +
                           entity.getComponent<BenchC>()->value;
                  return sum;
                }));
  record("finder-loop-3",
         count,
         bestNs(kRounds,
                repeats,
                three.size(),
                [&]
                {
                  uint64_t sum = 0;
                  const auto a = reg.finder<BenchA>();
                  const auto b = reg.finder<BenchB>();
                  const auto c = reg.finder<BenchC>();
                  for (const Entity& entity : three)
                  {
                    const Entity::EntityId id = entity.getId();
                    sum += a(id)->value + b(id)->value + c(id)->value;
                  }
                  return sum;
                }));

  record("getComponent-hit",
         count,
         bestNs(kRounds,
                repeats,
                world.withB.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : world.withB)
                    sum += entity.getComponent<BenchB>()->value;
                  return sum;
                }));
  record("hasComponent-miss",
         count,
         bestNs(kRounds,
                repeats,
                world.withoutB.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : world.withoutB)
                    sum += entity.hasComponent<BenchB>() ? 1 : 0;
                  return sum;
                }));
  record("getComponent-miss",
         count,
         bestNs(kRounds,
                repeats,
                world.withoutB.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : world.withoutB)
                    sum += entity.getComponent<BenchB>() ? 1 : 0;
                  return sum;
                }));
  record("has-get-pair",
         count,
         bestNs(kRounds,
                repeats,
                world.withB.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : world.withB)
                    // sfs-ignore-next-line(has-then-get)
                    if (entity.hasComponent<BenchB>())
                      sum += entity.getComponent<BenchB>()->value;
                  return sum;
                }));

  record("isValid",
         count,
         bestNs(kRounds,
                repeats,
                world.mixed.size(),
                [&]
                {
                  uint64_t sum = 0;
                  for (const Entity& entity : world.mixed)
                    sum += entity.isValid() ? 1 : 0;
                  return sum;
                }));
}

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
    lists.insert(
        &reg.query<Part<kCommon>, Part<a>, Excluding<Part<b>>>().entities());
  else if constexpr (I % 5 < 2)
    lists.insert(&reg.query<Part<kCommon>, Part<a>>().entities());
  else if constexpr (I % 4 == 0)
    lists.insert(&reg.query<Part<a>, Any<Part<b>, Part<c>>>().entities());
  else
    lists.insert(&reg.query<Part<a>, Part<b>>().entities());
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

// Toggles TPart on and off across the entities, twice a round so a round
// adds and removes it once each: the best round's ns per change.
template <typename TPart>
double timeToggles(std::vector<Entity>& entities, int rounds)
{
  return bestNs(rounds,
                1,
                2 * entities.size(),
                [&]
                {
                  for (int pass = 0; pass < 2; ++pass)
                    for (Entity& entity : entities)
                    {
                      if (entity.hasComponent<TPart>())
                        entity.removeComponent<TPart>();
                      else
                        entity.addComponent<TPart>();
                    }
                  return uint64_t{0};
                });
}

// An earlier run's results, from the file SFS_BENCH_BASELINE names (none:
// empty).
Results readBaseline()
{
  Results baseline;
  const char* path = std::getenv("SFS_BENCH_BASELINE");
  if (!path || !*path)
    return baseline;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line))
  {
    std::istringstream words(line);
    std::string tag;
    std::string name;
    int entities = 0;
    double ns = 0.0;
    if (words >> tag >> name >> entities >> ns && tag == "bench")
      baseline[{name, entities}] = ns;
  }
  if (baseline.empty())
    std::printf("  no results read from %s\n", path);
  return baseline;
}

// One gate, `now` at most `limit`: printed, and checked when both are known.
void gate(const std::string& what, int entities, double now, double limit)
{
  if (now <= 0.0 || limit <= 0.0)
  {
    std::printf("  gate %-56s %6d: not measured\n", what.c_str(), entities);
    return;
  }
  const bool met = now <= limit;
  std::printf("  gate %-56s %6d: %.3f against %.3f: %s\n",
              what.c_str(),
              entities,
              now,
              limit,
              met ? "met" : "MISSED");
  CHECK(met);
}

double resultOf(const Results& results, const std::string& name, int entities)
{
  const auto found = results.find({name, entities});
  return found == results.end() ? 0.0 : found->second;
}

// The entity API's speed gates (docs/core/ecs/index.md), against an earlier
// run's results where they compare with it.
void checkGates(const Results& before)
{
  if (before.empty())
  {
    std::printf("  no baseline (SFS_BENCH_BASELINE): gates not checked\n");
    return;
  }
  const Results& now = g_results;
  for (const int count : {2000, 50000})
  {
    const double vector = resultOf(now, "entity-loop-vector", count);
    const double view = resultOf(now, "entity-loop-view", count);
    gate("1 entity-only loop, view <= vector + 3%", count, view, vector * 1.03);
    gate("1 entity-only loop, view >= vector - 3%", count, vector * 0.97, view);
    gate("6 component-1 <= rows-1 + 5%",
         count,
         resultOf(now, "component-1", count),
         1.05 * resultOf(now, "rows-1", count));
    for (const std::string width : {"2", "3"})
    {
      const double rows = resultOf(now, "rows-" + width, count);
      gate("6 components-" + width + " <= rows-" + width + " + 5%",
           count,
           resultOf(now, "components-" + width, count),
           1.05 * rows);
      gate("2 rows-" + width + " <= 0.6 x today's getComponent loop",
           count,
           rows,
           0.6 * resultOf(before, "getComponent-loop-" + width, count));
      gate("2 rows-" + width + " <= the finder loop",
           count,
           rows,
           resultOf(now, "finder-loop-" + width, count));
    }
    gate("3 getComponent hit <= today's",
         count,
         resultOf(now, "getComponent-hit", count),
         resultOf(before, "getComponent-hit", count));
    gate("3 getComponent miss <= today's hasComponent miss + 10%",
         count,
         resultOf(now, "getComponent-miss", count),
         1.10 * resultOf(before, "hasComponent-miss", count));
    gate("4 isValid() <= 0.5 x today's isValid() && isAlive()",
         count,
         resultOf(now, "isValid", count),
         0.5 * resultOf(before, "isValid-and-isAlive", count));
  }
  // Only an upper bound: the smaller handle makes list inserts and erases
  // cheaper, so changes may well get faster.
  for (const std::string kind :
       {"change-named-by-none", "change-named-by-few", "change-named-by-many"})
    gate("5 " + kind + " <= today's + 5%",
         kEntities,
         resultOf(now, kind, kEntities),
         resultOf(before, kind, kEntities) * 1.05);
}

} // namespace

int main()
{
  const Results baseline = readBaseline();

  TEST("lookups and loops should cost no more than their fetches")
  {
    for (int pass = 0; pass < kPasses; ++pass)
    {
      benchLookups(2000);
      benchLookups(50000);
    }
  }

  TEST("one component change should cost little with many lists and systems")
  {
    for (int pass = 0; pass < kPasses; ++pass)
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
        giveParts(
            entities.back(), i, std::make_integer_sequence<int, kTypes>());
      }
      reg.flushEntities();

      constexpr int rounds = 20;
      // Warm up, then time each kind.
      timeToggles<Part<kUnnamed>>(entities, 1);
      record("change-named-by-none",
             kEntities,
             timeToggles<Part<kUnnamed>>(entities, rounds));
      record("change-named-by-few",
             kEntities,
             timeToggles<Part<kRare>>(entities, rounds));
      record("change-named-by-many",
             kEntities,
             timeToggles<Part<kCommon>>(entities, rounds));
      if (pass == 0)
        std::printf("  %zu lists, %d systems, %d entities\n",
                    lists.size(),
                    kSystems,
                    kEntities);

      // Every entity has kCommon again (an even number of passes).
      CHECK((reg.query<Part<kCommon>>().size() == kEntities));
      CHECK((reg.query<Part<kUnnamed>>().empty()));
      CHECK((reg.query<Part<kRare>>().empty()));
    }
  }

  printResults();
  TEST("the entity API should meet its speed gates") { checkGates(baseline); }

  return testing::report("ecsBenchTests");
}
