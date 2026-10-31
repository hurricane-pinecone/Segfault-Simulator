#include "../../engine/tests/testHarness.h"

#include "history/historySteps.h"

#include <string>

using nlohmann::json;

namespace
{
// A scene whose snapshot is about `size` bytes of text.
json scene(int value, std::size_t size = 0) { return {{"value", value}, {"padding", std::string(size, 'x')}}; }
} // namespace

int main()
{
  TEST("undo and redo should restore the scenes and selections recorded")
  {
    HistorySteps steps;
    steps.push(scene(0), scene(1), std::nullopt, 3u);
    steps.push(scene(1), scene(2), 3u, 4u);
    CHECK(steps.undoSteps() == 2u);

    auto restore = steps.undo();
    CHECK(restore.has_value());
    CHECK(restore->snapshot == scene(1));
    CHECK(restore->selection == std::optional<std::size_t>(3u));
    restore = steps.undo();
    CHECK(restore->snapshot == scene(0));
    CHECK(!restore->selection.has_value());
    CHECK(!steps.undo().has_value());

    restore = steps.redo();
    CHECK(restore->snapshot == scene(1));
    CHECK(restore->selection == std::optional<std::size_t>(3u));
    // A new edit drops what could be redone.
    steps.push(scene(1), scene(5), 3u, 3u);
    CHECK(!steps.canRedo());
    CHECK(steps.undo()->snapshot == scene(1));
    CHECK(steps.undo()->snapshot == scene(0));
  }

  TEST("snapshots should round-trip exactly, numbers and all")
  {
    const json snapshot = {{"position", {0.1, -2.5e-7, 1.0 / 3.0}},
                           {"count", 7},
                           {"negative", -3},
                           {"name", "Barn \"big\""},
                           {"flags", {true, false, nullptr}}};
    HistorySteps steps;
    steps.push(snapshot, scene(1), std::nullopt, std::nullopt);
    CHECK(steps.undo()->snapshot == snapshot);
  }

  TEST("the oldest steps should go past the step limit")
  {
    HistorySteps steps(3, std::size_t(1) << 30);
    for (int i = 0; i < 10; ++i)
      steps.push(scene(i), scene(i + 1), std::nullopt, std::nullopt);
    CHECK(steps.undoSteps() == 3u);
    CHECK(steps.undo()->snapshot == scene(9));
    CHECK(steps.undo()->snapshot == scene(8));
    CHECK(steps.undo()->snapshot == scene(7));
    CHECK(!steps.canUndo());
  }

  TEST("the oldest steps should go past the memory budget, the latest staying")
  {
    // Each step holds about 2 x 1000 bytes; a budget of 5000 keeps two.
    HistorySteps steps(200, 5000);
    for (int i = 0; i < 10; ++i)
      steps.push(scene(i, 1000), scene(i + 1, 1000), std::nullopt, std::nullopt);
    CHECK(steps.undoSteps() == 2u);
    CHECK(steps.bytes() <= 5000u);
    CHECK(steps.undo()->snapshot == scene(9, 1000));
    CHECK(steps.undo()->snapshot == scene(8, 1000));

    // One step bigger than the whole budget is still kept.
    HistorySteps tiny(200, 10);
    tiny.push(scene(0, 100), scene(1, 100), std::nullopt, std::nullopt);
    tiny.push(scene(1, 100), scene(2, 100), std::nullopt, std::nullopt);
    CHECK(tiny.undoSteps() == 1u);
    CHECK(tiny.undo()->snapshot == scene(1, 100));
  }

  TEST("amending should replace the last step's after, and clear should empty")
  {
    HistorySteps steps;
    steps.push(scene(0), scene(1), std::nullopt, 1u);
    steps.amendLast(scene(2), 2u);
    CHECK(steps.undo()->snapshot == scene(0));
    const auto redone = steps.redo();
    CHECK(redone->snapshot == scene(2));
    CHECK(redone->selection == std::optional<std::size_t>(2u));
    steps.clear();
    CHECK(!steps.canUndo());
    CHECK(!steps.canRedo());
    CHECK(steps.bytes() == 0u);
  }

  TEST("rewrite should change every recorded scene, undo and redo alike")
  {
    HistorySteps steps;
    steps.push(scene(0), scene(1), std::nullopt, std::nullopt);
    steps.push(scene(1), scene(2), std::nullopt, std::nullopt);
    steps.undo();
    steps.rewrite([](json& snapshot) { snapshot["renamed"] = snapshot["value"]; });
    json expected = scene(0);
    expected["renamed"] = 0;
    CHECK(steps.undo()->snapshot == expected);
    expected = scene(2);
    expected["renamed"] = 2;
    steps.redo();
    CHECK(steps.redo()->snapshot == expected);
  }

  TEST("an asset edit should undo and redo in its turn among the scene's")
  {
    HistorySteps steps;
    int painted = 0;
    steps.push(scene(0), scene(1), std::nullopt, std::nullopt);
    steps.pushAssetEdit(std::make_shared<const HistorySteps::AssetEdit>(
                            HistorySteps::AssetEdit{[&] { painted = 0; }, [&] { painted = 1; }, 64}),
                        2u);
    painted = 1;
    steps.push(scene(1), scene(2), std::nullopt, std::nullopt);

    CHECK(steps.undo()->snapshot == scene(1));
    auto restore = steps.undo();
    CHECK(restore->asset != nullptr);
    restore->asset->undo();
    CHECK(painted == 0);
    CHECK(steps.undo()->snapshot == scene(0));
    CHECK(steps.redo()->snapshot == scene(1));
    restore = steps.redo();
    CHECK(restore->asset != nullptr);
    restore->asset->redo();
    CHECK(painted == 1);
    CHECK(steps.redo()->snapshot == scene(2));
    CHECK(steps.bytes() >= 64u);
  }

  return testing::report("historyStepsTests");
}
