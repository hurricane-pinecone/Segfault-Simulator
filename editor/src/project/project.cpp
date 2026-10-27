#include "project/project.h"

#include "engine/core/logger/logger.h"
#include "nlohmann/json.hpp"

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
  return Project(resolveFolder(folder), std::move(name));
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

  LOG_INFO("Created project '" + name + "' at " + folder.string());
  return project;
}
