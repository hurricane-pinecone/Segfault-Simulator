#include "../../engine/tests/testHarness.h"

#include "project/project.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace
{
std::string read(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The .clangd editors wrote before sfs-lsp.
const std::string kOldClangd =
    "# clangd (the editor's Code panel, and most editors): the game module's\n"
    "# compile database, linked into .sfs/ by every build.\n"
    "CompileFlags:\n"
    "  CompilationDatabase: .sfs\n"
    "  Remove: [-fno-keep-inline-dllexport] # GCC-only (MinGW builds)\n";
} // namespace

int main()
{
  const fs::path work = fs::temp_directory_path() / "sfs-project-files-test";
  fs::remove_all(work);

  TEST("a new project's .clangd should silence unknown attributes for sfs-lsp "
       "to check")
  {
    const auto project = Project::create(work / "New", "New");
    CHECK(project.has_value());
    const std::string clangd = read(work / "New/.clangd");
    CHECK(clangd.starts_with(kOldClangd));
    CHECK(clangd.find("Diagnostics:\n  Suppress: [unknown-attributes]\n") !=
          std::string::npos);
  }

  TEST("a .clangd still as an older editor wrote it should be updated; an "
       "edited one kept")
  {
    const auto project = Project::create(work / "Old", "Old");
    CHECK(project.has_value());
    if (!project)
      continue;
    std::ofstream(work / "Old/.clangd", std::ios::binary) << kOldClangd;
    CHECK(project->ensureBuildFiles());
    CHECK(read(work / "Old/.clangd").find("Suppress: [unknown-attributes]") !=
          std::string::npos);

    const std::string edited = kOldClangd + "# mine\n";
    std::ofstream(work / "Old/.clangd", std::ios::binary) << edited;
    CHECK(project->ensureBuildFiles());
    CHECK(read(work / "Old/.clangd") == edited);
  }

  TEST("the SFS tree's .clangd should silence attributes as a project's does")
  {
    const auto project = Project::create(work / "Tree", "Tree");
    CHECK(project.has_value());
    const std::string clangd = read(work / "Tree/.clangd");
    // What a project's adds to the old default is the SFS tree's whole file.
    CHECK(clangd.size() > kOldClangd.size() &&
          clangd.substr(kOldClangd.size()) ==
              read(fs::path(SFS_SOURCE_DIR) / ".clangd"));
  }

  fs::remove_all(work);
  return testing::report("projectFilesTests");
}
