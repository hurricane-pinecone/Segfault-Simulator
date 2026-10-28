#include "../../engine/tests/testHarness.h"

#include "platform/paths.h"
#include "scripting/scriptFiles.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
struct TempFolder
{
  fs::path path;
  explicit TempFolder(const char* name) : path(fs::temp_directory_path() / name)
  {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempFolder() { fs::remove_all(path); }
};

void write(const fs::path& file, const std::string& text)
{
  fs::create_directories(file.parent_path());
  std::ofstream(file) << text;
}

std::string read(const fs::path& file)
{
  std::ifstream in(file);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
} // namespace

int main()
{
  TEST("path helpers should keep a network share's prefix and fold . and ..")
  {
    CHECK(paths::normal("//wsl.localhost/Ubuntu/home/a/./b/../c") == "//wsl.localhost/Ubuntu/home/a/c");
    CHECK(paths::normal("/home/a//b/") == "/home/a/b");
    CHECK(paths::relative("//host/share/p/scripts/x/y.h", "//host/share/p/scripts") == std::optional<std::string>("x/y.h"));
    CHECK(paths::relative("//host/share/p/scripts", "//host/share/p/scripts/") == std::optional<std::string>(""));
    CHECK(!paths::relative("//host/share/p/scriptsX/y.h", "//host/share/p/scripts"));
    CHECK(paths::moved("//h/s/a/b/c.h", "//h/s/a/b", "//h/s/a/z") == fs::path("//h/s/a/z/c.h"));
    CHECK(paths::moved("//h/s/q.h", "//h/s/a", "//h/s/z") == fs::path("//h/s/q.h"));
  }

  TEST("a script's file should be found by its class name, source or header first")
  {
    TempFolder scripts("sfs_script_files_find");
    write(scripts.path / "systems/CropSystem.h", "");
    write(scripts.path / "systems/CropSystem.cpp", "");
    write(scripts.path / "data/CropType.h", "");
    using scriptFiles::Prefer;
    CHECK(scriptFiles::find(scripts.path, "CropSystem", Prefer::Source)->filename() == "CropSystem.cpp");
    CHECK(scriptFiles::find(scripts.path, "cropsystem", Prefer::Header)->filename() == "CropSystem.h");
    CHECK(scriptFiles::find(scripts.path, "CropType", Prefer::Source)->filename() == "CropType.h"); // only one
    CHECK(!scriptFiles::find(scripts.path, "Missing", Prefer::Source));
  }

  TEST("moving a scripts folder should carry the scripts' includes of it along")
  {
    TempFolder scripts("sfs_script_files_folder");
    write(scripts.path / "data/CropType.h", "#pragma once\n");
    write(scripts.path / "database/Store.h", "#pragma once\n");
    write(scripts.path / "systems/CropSystem.cpp",
          "#include \"systems/CropSystem.h\"\n#include \"data/CropType.h\"\n#include \"database/Store.h\"\n"
          "#include <sfs/sfs.h>\n");
    write(scripts.path / "systems/CropSystem.h", "#pragma once\n");

    CHECK(scriptFiles::move(scripts.path, {{scripts.path / "data", scripts.path / "templates"}}));
    CHECK(fs::exists(scripts.path / "templates/CropType.h") && !fs::exists(scripts.path / "data"));
    CHECK(read(scripts.path / "systems/CropSystem.cpp") ==
          "#include \"systems/CropSystem.h\"\n#include \"templates/CropType.h\"\n#include \"database/Store.h\"\n"
          "#include <sfs/sfs.h>\n");

    // Not onto something that's there.
    CHECK(!scriptFiles::move(scripts.path, {{scripts.path / "templates", scripts.path / "systems"}}));
    CHECK(fs::exists(scripts.path / "templates/CropType.h"));
  }

  TEST("moving a script should fix includes beside it and of it")
  {
    TempFolder scripts("sfs_script_files_file");
    write(scripts.path / "objects/Spin.h", "#pragma once\n");
    write(scripts.path / "objects/Spin.cpp", "#include \"Spin.h\"\n");
    write(scripts.path / "objects/Mill.cpp", "#include \"objects/Spin.h\"\n#include \"Spin.h\"\n");

    // The header moves away from its source: both find it again.
    CHECK(scriptFiles::move(scripts.path, {{scripts.path / "objects/Spin.h", scripts.path / "spin/Spin.h"}}));
    CHECK(read(scripts.path / "objects/Spin.cpp") == "#include \"spin/Spin.h\"\n");
    CHECK(read(scripts.path / "objects/Mill.cpp") == "#include \"spin/Spin.h\"\n#include \"spin/Spin.h\"\n");

    // Moved together, a pair's include of its own header still works as it was.
    CHECK(scriptFiles::move(scripts.path, {{scripts.path / "spin/Spin.h", scripts.path / "objects/Turn.h"},
                                           {scripts.path / "objects/Spin.cpp", scripts.path / "objects/Turn.cpp"}}));
    CHECK(read(scripts.path / "objects/Turn.cpp") == "#include \"objects/Turn.h\"\n");

    // A pair renamed in place: its include of its own header stays beside it.
    write(scripts.path / "objects/Gear.h", "#pragma once\n");
    write(scripts.path / "objects/Gear.cpp", "#include \"Gear.h\"\n");
    CHECK(scriptFiles::move(scripts.path, {{scripts.path / "objects/Gear.h", scripts.path / "objects/Cog.h"},
                                           {scripts.path / "objects/Gear.cpp", scripts.path / "objects/Cog.cpp"}}));
    CHECK(read(scripts.path / "objects/Cog.cpp") == "#include \"Cog.h\"\n");
  }

  TEST("copying a script should make a class of its own, with its pair")
  {
    TempFolder scripts("sfs_script_files_copy");
    write(scripts.path / "objects/Spin.h", "#pragma once\n// Spin turns.\nclass Spin {};\n");
    write(scripts.path / "objects/Spin.cpp", "#include \"Spin.h\"\nvoid f(Spin&);\n");
    write(scripts.path / "objects/Mill.cpp", "#include \"objects/Spin.h\"\n");

    // Duplicated beside itself: SpinCopy.h/.cpp, the class renamed in the copies only.
    const auto made = scriptFiles::copy(scripts.path, scripts.path / "objects/Spin.h", scripts.path / "objects");
    CHECK(made.size() == 2u);
    CHECK(read(scripts.path / "objects/SpinCopy.h") == "#pragma once\n// Spin turns.\nclass SpinCopy {};\n");
    CHECK(read(scripts.path / "objects/SpinCopy.cpp") == "#include \"SpinCopy.h\"\nvoid f(SpinCopy&);\n");
    CHECK(read(scripts.path / "objects/Spin.h") == "#pragma once\n// Spin turns.\nclass Spin {};\n");
    // Again: SpinCopy2.
    scriptFiles::copy(scripts.path, scripts.path / "objects/Spin.cpp", scripts.path / "objects");
    CHECK(fs::exists(scripts.path / "objects/SpinCopy2.h") && fs::exists(scripts.path / "objects/SpinCopy2.cpp"));

    // A folder pasted elsewhere: its classes renamed, its includes of itself
    // pointing at the copies, others left as they were.
    TempFolder more("sfs_script_files_copy_folder");
    write(more.path / "gear/Gear.h", "#pragma once\n#include \"util/Maths.h\"\nclass Gear {};\n");
    write(more.path / "gear/Use.cpp", "#include \"gear/Gear.h\"\nGear g;\n");
    write(more.path / "util/Maths.h", "#pragma once\n");
    const auto folder = scriptFiles::copy(more.path, more.path / "gear", more.path / "util");
    CHECK(folder.size() == 1u && folder[0] == more.path / "util/gear");
    CHECK(read(more.path / "util/gear/GearCopy.h") == "#pragma once\n#include \"util/Maths.h\"\nclass GearCopy {};\n");
    CHECK(read(more.path / "util/gear/Use.cpp") == "#include \"util/gear/GearCopy.h\"\nGearCopy g;\n");
  }

  TEST("renaming an identifier should leave comments and strings alone")
  {
    TempFolder scripts("sfs_script_files_identifier");
    write(scripts.path / "Spin.h",
          "// Spin: turns things. Spinner stays.\nclass Spin : public sfs::GameObject {};\n"
          "inline const char* name = \"Spin\"; Spin* SpinPtr; Spin s;\n");
    CHECK(scriptFiles::renameIdentifier(scripts.path, "Spin", "Turn") == 1);
    CHECK(read(scripts.path / "Spin.h") ==
          "// Spin: turns things. Spinner stays.\nclass Turn : public sfs::GameObject {};\n"
          "inline const char* name = \"Spin\"; Turn* SpinPtr; Turn s;\n");
  }

  return testing::report("scriptFilesTests");
}
