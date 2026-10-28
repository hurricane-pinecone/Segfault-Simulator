#include "project/project.h"

#include "engine/core/logger/logger.h"
#include "nlohmann/json.hpp"

#include <cctype>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
// Bumped when the project file's layout changes, so an older editor can refuse
// a project it doesn't understand.
constexpr int kFormatVersion = 1;

// Anchor a relative folder at the working directory; leave anything rooted
// (a drive, a UNC share, or a leading slash) alone. MinGW's libstdc++ neither
// reports UNC paths as absolute nor resolves them correctly in fs::absolute
// (it re-prefixes the share), so test for any root instead of is_absolute().
fs::path resolveFolder(const fs::path& folder)
{
  if (folder.has_root_name() || folder.has_root_directory())
    return folder;
  return fs::current_path() / folder;
}
} // namespace

std::optional<Project> Project::open(const fs::path& folder)
{
  const fs::path file = folder / kFileName;
  std::ifstream in(file);
  if (!in)
  {
    LOG_ERROR("No project file at " + file.string());
    return std::nullopt;
  }

  const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
  if (json.is_discarded() || !json.is_object())
  {
    LOG_ERROR("Project file is not valid JSON: " + file.string());
    return std::nullopt;
  }

  if (json.value("formatVersion", 0) > kFormatVersion)
  {
    LOG_ERROR("Project " + file.string() + " needs a newer editor");
    return std::nullopt;
  }

  std::string name = json.value("name", folder.filename().string());
  LOG_INFO("Opened project '" + name + "' at " + folder.string());
  Project project(resolveFolder(folder), std::move(name));
  project.m_startScene = json.value("startScene", std::string());
  return project;
}

bool Project::setStartScene(const fs::path& scene)
{
  // Relative to the project, by prefix: MinGW mis-parses UNC paths, which
  // lexically_relative would trip over.
  fs::path relative = scene;
  const std::string root = m_root.string();
  const std::string path = scene.string();
  if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
      (path[root.size()] == '/' || path[root.size()] == '\\'))
    relative = fs::path(path.substr(root.size() + 1));

  // Read-modify-write, so fields this editor doesn't know survive.
  const fs::path file = m_root / kFileName;
  nlohmann::json json = nlohmann::json::object();
  if (std::ifstream in(file); in)
  {
    json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object())
      json = nlohmann::json::object();
  }
  json["formatVersion"] = kFormatVersion;
  json["name"] = m_name;
  json["startScene"] = relative.generic_string();

  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << json.dump(2) << '\n';
  if (!out)
  {
    LOG_ERROR("Can't write " + file.string());
    return false;
  }
  m_startScene = relative;
  LOG_INFO("Start scene: " + relative.generic_string());
  return true;
}

std::string Project::moduleName() const
{
  std::string name;
  for (const char c : m_name)
    name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
    name.insert(0, "Game");
  return name;
}

namespace
{
bool writeIfMissing(const fs::path& path, const std::string& contents)
{
  if (fs::exists(path))
    return true;
  std::ofstream out(path, std::ios::binary);
  out << contents;
  if (!out)
  {
    LOG_ERROR("Can't write " + path.string());
    return false;
  }
  LOG_INFO("Created " + path.filename().string());
  return true;
}
} // namespace

bool Project::ensureBuildFiles() const
{
  const std::string cmake =
      "# Builds this project's scripts (scripts/) into its game module, the\n"
      "# library the editor loads to play the game. Open the folder in any\n"
      "# CMake-aware IDE to work on scripts outside the editor too.\n"
      "cmake_minimum_required(VERSION 3.24)\n"
      "project(" + moduleName() + " LANGUAGES C CXX)\n\n"
      "set(CMAKE_CXX_STANDARD 23)\n"
      "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
      "set(CMAKE_CXX_EXTENSIONS OFF)\n"
      "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\n"
      "find_package(engine REQUIRED)\n\n"
      "sfs_add_game_module(" + moduleName() + ")\n";

  const std::string gitignore =
      "# Editor build output and caches\n"
      ".sfs/\n"
      "builds/\n"
      "compile_commands.json\n"
      "imgui.ini\n";

  const std::string clangd =
      "# clangd (the editor's Code panel, and most editors): the game module's\n"
      "# compile database, linked into .sfs/ by every build.\n"
      "CompileFlags:\n"
      "  CompilationDatabase: .sfs\n"
      "  Remove: [-fno-keep-inline-dllexport] # GCC-only (MinGW builds)\n";

  std::error_code error;
  fs::create_directories(scriptsFolder(), error);
  return writeIfMissing(m_root / "CMakeLists.txt", cmake) &&
         writeIfMissing(m_root / ".gitignore", gitignore) &&
         writeIfMissing(m_root / ".clangd", clangd);
}

std::optional<Project> Project::create(const fs::path& folder,
                                       const std::string& name)
{
  const fs::path file = folder / kFileName;
  if (fs::exists(file))
  {
    LOG_ERROR("A project already exists at " + folder.string());
    return std::nullopt;
  }

  Project project(resolveFolder(folder), name);

  std::error_code error;
  for (const fs::path& sub : {project.scenesFolder(),
                              project.assetsFolder(),
                              project.prefabsFolder(),
                              project.scriptsFolder()})
  {
    fs::create_directories(sub, error);
    if (error)
    {
      LOG_ERROR("Can't create " + sub.string() + ": " + error.message());
      return std::nullopt;
    }
  }

  std::ofstream out(file);
  out << nlohmann::json{{"formatVersion", kFormatVersion}, {"name", name}}.dump(2)
      << '\n';
  if (!out)
  {
    LOG_ERROR("Can't write " + file.string());
    return std::nullopt;
  }
  if (!project.ensureBuildFiles())
    return std::nullopt;

  LOG_INFO("Created project '" + name + "' at " + folder.string());
  return project;
}
