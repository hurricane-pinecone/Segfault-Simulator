#include "../../testHarness.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/ecs/sceneData.h"
#include "engine/core/logger/logger.h"

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Scene data (docs/core/ecs): types marked [[sfs::scene_data]], of which the
// registry holds at most one each, apart from every entity. sfs-codegen writes
// their traits.

using namespace sfs;

namespace
{

struct [[sfs::scene_data]] Scoreboard
{
  int home = 0;
  int away = 0;
};

struct [[sfs::scene_data]] MatchRules
{
  MatchRules() = default;
  MatchRules(float minutes, std::string arena)
      : minutes(minutes), arena(std::move(arena))
  {
  }

  float minutes = 90.0f;
  std::string arena;
};

// Shares a count with whoever keeps a copy of `alive`, to tell it's destroyed.
struct [[sfs::scene_data]] Tracked
{
  std::shared_ptr<int> alive = std::make_shared<int>(0);
};

struct Health
{
  int value = 0;
};

static_assert(detail::kSceneData<Scoreboard>);
static_assert(detail::kSceneData<const Scoreboard>);
static_assert(!detail::kSceneData<Health>);

// The errors logged while it lives.
struct Errors
{
  std::vector<std::string> lines;
  Logger::SinkId sink;

  Errors()
  {
    Logger::flush();
    sink = Logger::addSink(
        [this](Logger::Level level, const std::string& message)
        {
          if (level == Logger::Level::ERROR)
            lines.push_back(message);
        });
  }
  ~Errors()
  {
    Logger::removeSink(sink);
    Logger::flush();
  }
  Errors(const Errors&) = delete;
  Errors& operator=(const Errors&) = delete;

  int count(std::string_view part)
  {
    Logger::flush();
    int found = 0;

    for (const std::string& line : lines)
      found += line.find(part) != std::string::npos ? 1 : 0;

    return found;
  }
};

} // namespace

int main()
{
  TEST("sceneData should be null until it's added, then the one added")
  {
    Registry reg;
    CHECK(reg.sceneData<Scoreboard>() == nullptr);
    Scoreboard& added = reg.addSceneData<Scoreboard>(Scoreboard{2, 1});
    CHECK(reg.sceneData<Scoreboard>() == &added);
    CHECK(added.home == 2 && added.away == 1);
    // Another type has its own slot.
    CHECK(reg.sceneData<MatchRules>() == nullptr);
    MatchRules& rules = reg.addSceneData<MatchRules>(45.0f, "Docks");
    CHECK(rules.minutes == 45.0f && rules.arena == "Docks");
    CHECK(reg.sceneData<Scoreboard>() == &added);

    // Read through const, from the same slot.
    const Scoreboard* read = reg.sceneData<const Scoreboard>();
    CHECK(read == &added);
    static_assert(std::is_same_v<decltype(reg.sceneData<const Scoreboard>()),
                                 const Scoreboard*>);
  }

  TEST("scene data should stay put as entities and other data come and go")
  {
    Registry reg;
    Scoreboard& board = reg.addSceneData<Scoreboard>();

    for (int i = 0; i < 100; ++i)
      reg.createEntity().addComponent<Health>(Health{i});

    reg.flushEntities();
    reg.addSceneData<MatchRules>();
    CHECK(reg.sceneData<Scoreboard>() == &board);
    CHECK(reg.entities().size() == 100);
  }

  TEST("a second addSceneData should be refused and logged, keeping the first")
  {
    Registry reg;
    Errors errors;
    Scoreboard& first = reg.addSceneData<Scoreboard>(Scoreboard{3, 0});
    Scoreboard& again = reg.addSceneData<Scoreboard>(Scoreboard{9, 9});
    CHECK(&again == &first);
    CHECK(first.home == 3);
    CHECK(errors.count("addSceneData<") == 1);
    CHECK(errors.count("Scoreboard>: the scene has one already") == 1);
  }

  TEST("removeSceneData should destroy it at once, and it may be added again")
  {
    Registry reg;
    Errors errors;
    reg.removeSceneData<Tracked>(); // none: nothing
    const std::shared_ptr<int> alive = reg.addSceneData<Tracked>().alive;
    CHECK(alive.use_count() == 2);
    reg.removeSceneData<Tracked>();
    CHECK(alive.use_count() == 1);
    CHECK(reg.sceneData<Tracked>() == nullptr);
    reg.addSceneData<Tracked>();
    CHECK(reg.sceneData<Tracked>() != nullptr);
    CHECK(errors.count("addSceneData") == 0);
  }

  TEST("the registry should destroy its scene data with itself")
  {
    std::shared_ptr<int> alive;
    {
      Registry reg;
      alive = reg.addSceneData<Tracked>().alive;
      CHECK(alive.use_count() == 2);
    }
    CHECK(alive.use_count() == 1);
  }

  return testing::report("sceneDataTests");
}
