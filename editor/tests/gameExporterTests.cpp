#include "../../engine/tests/testHarness.h"
#include "build/gameExporter.h"
#include "engine/runtime/game/gameManifest.h"
#include "project/project.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
void touch(const fs::path& path, const char* contents = "x")
{
  fs::create_directories(path.parent_path());
  std::ofstream(path) << contents;
}

#if defined(_WIN32)
constexpr const char* PLAYER = "SfsPlayer.exe";
constexpr const char* LIBRARY = "libengine.dll";
constexpr const char* MODULE = "MyGame.dll";
#else
constexpr const char* PLAYER = "SfsPlayer";
constexpr const char* LIBRARY = "libengine.so";
constexpr const char* MODULE = "MyGame.so";
#endif
} // namespace

int main()
{
  const fs::path root = fs::temp_directory_path() / "sfs-export-test";
  fs::remove_all(root);

  // A stand-in editor folder: the player, a library, an engine asset.
  const fs::path editor = root / "editor";
  touch(editor / PLAYER);
  touch(editor / LIBRARY);
  touch(editor / "assets" / "fonts" / "engine.ttf");
  touch(editor / "SfsEditor.log"); // not a library: stays behind

  std::optional<Project> project =
      Project::create(root / "project", "My Game!");
  touch(root / "project" / "scenes" / "main.scene", "{}");
  touch(root / "project" / "assets" / "tree.png");
  touch(root / "built" / MODULE);

  GameExporter::Options options;
  options.output = root / "out";
  options.startScene = "scenes/main.scene";
  options.module = root / "built" / MODULE;
  options.windowWidth = 1280;
  options.windowHeight = 720;
  const GameExporter exporter(editor);

  TEST(
      "an export should hold the renamed player, libraries, module and content")
  {
    CHECK(project.has_value());
    const fs::path executable = exporter.run(*project, options);
    CHECK(executable ==
          options.output / GameExporter::executableName("My Game!"));
    CHECK(fs::exists(executable));
    CHECK(fs::exists(options.output / LIBRARY));
    CHECK(fs::exists(options.output / MODULE));
    CHECK(fs::exists(options.output / "scenes" / "main.scene"));
    CHECK(fs::exists(options.output / "assets" / "tree.png"));
    CHECK(fs::exists(options.output / "assets" / "fonts" / "engine.ttf"));
    CHECK(!fs::exists(options.output / "SfsEditor.log"));
  }

  TEST("game.json should describe the game")
  {
    const std::optional<sfs::GameManifest> manifest =
        sfs::GameManifest::read(options.output);
    CHECK(manifest.has_value());
    CHECK(manifest->name == std::string("My Game!"));
    CHECK(manifest->moduleName == project->moduleName());
    CHECK(manifest->module == fs::path(MODULE));
    CHECK(manifest->startScene == fs::path("scenes/main.scene"));
    CHECK(manifest->windowWidth == 1280);
    CHECK(manifest->windowHeight == 720);
  }

  TEST("exporting again should replace the previous export")
  {
    touch(options.output / "stale.txt");
    CHECK(!exporter.run(*project, options).empty());
    CHECK(!fs::exists(options.output / "stale.txt"));
  }

  TEST("a folder that isn't an export should never be overwritten")
  {
    GameExporter::Options unsafe = options;
    unsafe.output = root / "documents";
    touch(unsafe.output / "important.txt");
    CHECK(exporter.run(*project, unsafe).empty());
    CHECK(fs::exists(unsafe.output / "important.txt"));
  }

  TEST("an export without a start scene or module should fail")
  {
    GameExporter::Options noScene = options;
    noScene.startScene = "scenes/missing.scene";
    CHECK(exporter.run(*project, noScene).empty());
    GameExporter::Options noModule = options;
    noModule.module = root / "built" / "missing.so";
    CHECK(exporter.run(*project, noModule).empty());
  }

  TEST("the start scene should be saved with the project")
  {
    CHECK(project->setStartScene(root / "project" / "scenes" / "main.scene"));
    const std::optional<Project> reopened = Project::open(root / "project");
    CHECK(reopened && reopened->startScene() == fs::path("scenes/main.scene"));
  }

  fs::remove_all(root);
  return testing::report("gameExporterTests");
}
