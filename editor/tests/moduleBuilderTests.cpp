#include "../../engine/tests/testHarness.h"

#include "build/moduleBuilder.h"

#if !defined(_WIN32)
  #include "build/buildEnvironment.h"
  #include "project/project.h"

  #include <chrono>
  #include <cstdlib>
  #include <optional>
  #include <sys/stat.h>
  #include <thread>
#endif

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
void write(const fs::path& path, const std::string& contents)
{
  fs::create_directories(path.parent_path());
  std::ofstream(path) << contents;
}

bool listed(const std::vector<fs::path>& files, const fs::path& file)
{
  for (const fs::path& each : files)
    if (each == file)
      return true;
  return false;
}
} // namespace

int main()
{
  const fs::path root = fs::temp_directory_path() / "sfs-module-builder-test";
  fs::remove_all(root);

  TEST("the sources saved after a build began should be the ones changedSince() lists")
  {
    const fs::path scripts = root / "changed" / "scripts";
    const fs::path stamp = root / "changed" / "build" / ".build-started";
    write(scripts / "Old.h", "struct Old {};");
    write(scripts / "nested" / "Saved.h", "struct Saved {};");
    write(scripts / "Saved.cpp", "");
    write(root / "changed" / "CMakeLists.txt", "");
    write(stamp, "");

    const fs::file_time_type began = fs::last_write_time(stamp);
    fs::last_write_time(scripts / "Old.h", began - std::chrono::seconds(10));
    fs::last_write_time(root / "changed" / "CMakeLists.txt", began - std::chrono::seconds(10));
    fs::last_write_time(scripts / "nested" / "Saved.h", began + std::chrono::seconds(2));
    fs::last_write_time(scripts / "Saved.cpp", began + std::chrono::milliseconds(1));

    const std::vector<fs::path> changed =
        ModuleBuilder::changedSince(stamp, {scripts, root / "changed" / "CMakeLists.txt"});
    CHECK(changed.size() == 2u);
    CHECK(listed(changed, scripts / "nested" / "Saved.h"));
    CHECK(listed(changed, scripts / "Saved.cpp"));
    CHECK(!listed(changed, scripts / "Old.h"));

    // A changed single file (not a folder) is listed too.
    fs::last_write_time(root / "changed" / "CMakeLists.txt", began + std::chrono::seconds(1));
    CHECK(ModuleBuilder::changedSince(stamp, {root / "changed" / "CMakeLists.txt"}).size() == 1u);
  }

  TEST("without a stamp (no build began) nothing should count as changed")
  {
    write(root / "nostamp" / "scripts" / "A.h", "");
    CHECK(ModuleBuilder::changedSince(root / "nostamp" / ".build-started", {root / "nostamp" / "scripts"}).empty());
  }

#if !defined(_WIN32)
  // The race the game hit (2026-11-04): a header saved while a file that
  // includes it compiled. That file's object holds the old struct but is newer
  // than the header, so the build tool never compiles it again, and the two
  // files disagree on the struct's size. Here the compiler launcher saves the
  // header during a.cpp's compile; check.cpp (built after) sees the new one.
  TEST("a header saved mid-build should leave no object compiled from the old one")
  {
    const fs::path folder = root / "race";
    std::optional<Project> project = Project::create(folder, "Race");
    CHECK(project.has_value());
    if (project)
    {
      write(folder / "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(Race LANGUAGES CXX)\n"
            "set(CMAKE_CXX_COMPILER_LAUNCHER \"${CMAKE_CURRENT_SOURCE_DIR}/tools/launch.sh\")\n"
            "add_library(a STATIC scripts/a.cpp)\n"
            "add_executable(check scripts/check.cpp)\n"
            "target_link_libraries(check PRIVATE a)\n");
      write(folder / "scripts" / "shape.h", "#pragma once\nstruct Shape { int x; };\n");
      write(folder / "scripts" / "a.cpp", "#include \"shape.h\"\n#include <cstddef>\nstd::size_t sizeA() { return sizeof(Shape); }\n");
      write(folder / "scripts" / "check.cpp",
            "#include \"shape.h\"\n#include <cstddef>\nstd::size_t sizeA();\n"
            "int main() { return sizeA() == sizeof(Shape) ? 0 : 1; }\n");
      // Compiles, then (once, for a.cpp) the header's saved bigger; the object
      // is written after (newer than the header), as when the save lands
      // while the compiler's still going.
      const fs::path launcher = folder / "tools" / "launch.sh";
      write(launcher,
            "#!/bin/sh\n"
            "\"$@\" || exit $?\n"
            "case \"$*\" in *a.cpp*)\n"
            "  if [ ! -f \"" + (folder / "saved").string() + "\" ]; then\n"
            "    touch \"" + (folder / "saved").string() + "\"\n"
            "    sed -i 's/int x;/int x; double more[8];/' \"" + (folder / "scripts" / "shape.h").string() + "\"\n"
            "    sleep 0.05\n"
            "    while [ $# -gt 0 ]; do [ \"$1\" = -o ] && touch \"$2\"; shift; done\n"
            "  fi;;\n"
            "esac\n");
      ::chmod(launcher.c_str(), 0755);

      ModuleBuilder builder(BuildEnvironment::current());
      // An editor on a static engine refuses to build one at all.
      if (!BuildEnvironment::current().engineShared())
      {
        CHECK(!builder.start(*project));
        fs::remove_all(root);
        return testing::report("moduleBuilderTests");
      }
      CHECK(builder.start(*project));
      std::optional<bool> built;
      for (int i = 0; i < 3000 && !(built = builder.poll()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(built == std::optional<bool>(true));
      CHECK(fs::exists(folder / "saved"));

      const fs::path check = project->buildFolder() / BuildEnvironment::current().platformName() / "check";
      CHECK(fs::exists(check));
      CHECK(std::system(check.c_str()) == 0); // both files agree on sizeof(Shape)
    }
  }
#endif

  fs::remove_all(root);
  return testing::report("moduleBuilderTests");
}
