#include "build/gameExporter.h"

#include "project/project.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/game/gameManifest.h"

#include <cctype>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
#if defined(_WIN32)
constexpr const char* kExecutableSuffix = ".exe";
#else
constexpr const char* kExecutableSuffix = "";
#endif

constexpr const char* kPlayerName = "SfsPlayer";

bool isSharedLibrary(const fs::path& path)
{
#if defined(_WIN32)
  return path.extension() == ".dll";
#else
  return path.filename().string().find(".so") != std::string::npos;
#endif
}

// Whether `folder` may be replaced by an export: missing, empty, or one.
bool replaceable(const fs::path& folder)
{
  std::error_code error;
  if (!fs::exists(folder, error))
    return true;
  return fs::is_empty(folder, error) || fs::exists(folder / sfs::GameManifest::kFileName, error);
}

// Copy a file, or a folder's contents merged into `to`. Explicit rather than
// fs::copy(recursive), which MinGW refuses when `to` already exists.
bool copyInto(const fs::path& from, const fs::path& to)
{
  std::error_code error;
  const auto copyFile = [&](const fs::path& source, const fs::path& target)
  {
    fs::create_directories(target.parent_path(), error);
    if (!error)
      fs::copy_file(source, target, fs::copy_options::overwrite_existing, error);
    return !error;
  };

  bool ok = true;
  if (fs::is_directory(from))
  {
    for (auto it = fs::recursive_directory_iterator(from, error);
         ok && !error && it != fs::recursive_directory_iterator(); it.increment(error))
      if (it->is_regular_file())
        ok = copyFile(it->path(), to / it->path().lexically_relative(from));
  }
  else
    ok = copyFile(from, to);

  if (!ok || error)
  {
    LOG_ERROR("Can't copy " + from.string() + ": " + error.message());
    return false;
  }
  return true;
}
} // namespace

GameExporter::GameExporter(fs::path editorFolder) : m_editorFolder(std::move(editorFolder)) {}

std::string GameExporter::executableName(const std::string& projectName)
{
  std::string name;
  for (const char c : projectName)
    name += std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' ? c : '_';
  if (name.empty())
    name = "Game";
  return name + kExecutableSuffix;
}

std::vector<fs::path> GameExporter::runtimeLibraries() const
{
  std::vector<fs::path> libraries;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(m_editorFolder, error))
    if (entry.is_regular_file() && isSharedLibrary(entry.path()))
      libraries.push_back(entry.path());

  std::ifstream listed(m_editorFolder / (std::string(kPlayerName) + ".libs"));
  for (std::string line; std::getline(listed, line);)
    if (!line.empty() && fs::exists(line, error))
      libraries.emplace_back(line);
  return libraries;
}

fs::path GameExporter::run(const Project& project, const Options& options) const
{
  const fs::path player = m_editorFolder / (std::string(kPlayerName) + kExecutableSuffix);
  if (!fs::exists(player))
  {
    LOG_ERROR("Can't export: the player (" + player.string() + ") is missing");
    return {};
  }
  if (!fs::exists(options.module))
  {
    LOG_ERROR("Can't export: the game module hasn't been built");
    return {};
  }
  if (options.startScene.empty() || !fs::exists(project.root() / options.startScene))
  {
    LOG_ERROR("Can't export: choose a start scene");
    return {};
  }
  if (!replaceable(options.output))
  {
    LOG_ERROR("Can't export into " + options.output.string() +
              ": it isn't empty and isn't a previous export");
    return {};
  }

  std::error_code error;
  fs::remove_all(options.output, error);
  fs::create_directories(options.output, error);
  if (error)
  {
    LOG_ERROR("Can't create " + options.output.string() + ": " + error.message());
    return {};
  }

  // game.json first: it marks the folder as an export, so a failed export can
  // still be replaced by the next one.
  sfs::GameManifest manifest;
  manifest.name = project.name();
  manifest.moduleName = project.moduleName();
  manifest.module = options.module.filename();
  manifest.startScene = options.startScene;
  manifest.windowWidth = options.windowWidth;
  manifest.windowHeight = options.windowHeight;
  manifest.development = options.development;
  if (!manifest.write(options.output))
    return {};

  const fs::path executable = options.output / executableName(project.name());
  bool ok = copyInto(player, executable);
  for (const fs::path& library : runtimeLibraries())
    ok = ok && copyInto(library, options.output / library.filename());
  ok = ok && copyInto(options.module, options.output / options.module.filename());

  // The engine's own assets (fonts) first, so the project's can override them.
  if (fs::exists(m_editorFolder / "assets"))
    ok = ok && copyInto(m_editorFolder / "assets", options.output / "assets");
  for (const fs::path& folder : {project.scenesFolder(), project.assetsFolder(), project.prefabsFolder()})
    if (fs::exists(folder))
      ok = ok && copyInto(folder, options.output / folder.filename());
  if (!ok)
    return {};

  LOG_INFO("Exported " + project.name() + " to " + options.output.string());
  return executable;
}
