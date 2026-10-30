#include "../../testHarness.h"

#include <engine/runtime/game/gameManifest.h>

#include <filesystem>
#include <fstream>
#include <optional>

using namespace sfs;
namespace fs = std::filesystem;

int main()
{
  const fs::path folder = fs::temp_directory_path() / "sfsGameManifestTests";
  fs::remove_all(folder);
  fs::create_directories(folder);

  TEST("a game should have vsync on and no frame cap unless it says")
  {
    std::ofstream(folder / GameManifest::kFileName)
        << R"({"formatVersion": 1, "name": "Old", "startScene": "a.scene"})";
    const std::optional<GameManifest> manifest = GameManifest::read(folder);
    CHECK(manifest.has_value());
    CHECK(manifest && manifest->vsync);
    CHECK(manifest && manifest->maxFrameRate == 0.0);
  }

  TEST("vsync and the frame cap should be written and read back")
  {
    GameManifest written;
    written.name = "Game";
    written.startScene = "scenes/main.scene";
    written.vsync = false;
    written.maxFrameRate = 144.0;
    CHECK(written.write(folder));
    const std::optional<GameManifest> read = GameManifest::read(folder);
    CHECK(read.has_value());
    CHECK(read && !read->vsync);
    CHECK(read && read->maxFrameRate == 144.0);
  }

  fs::remove_all(folder);
  return testing::report("gameManifestTests");
}
