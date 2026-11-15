#include "build/headlessExport.h"

#include "build/buildEnvironment.h"
#include "build/fillParts.h"
#include "build/gameExporter.h"
#include "build/moduleBuilder.h"
#include "project/project.h"
#include "scene/sceneDocument.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/scripting/moduleHost.h"

#include <SDL_filesystem.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

namespace
{
// The project's start scene, else its first scene (by name); empty if none.
fs::path startScene(const Project& project)
{
  if (!project.startScene().empty())
    return project.startScene();

  std::vector<fs::path> scenes;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(project.scenesFolder(), error))
    if (entry.path().extension() == SceneDocument::kExtension)
      scenes.push_back(fs::path("scenes") / entry.path().filename());
  std::sort(scenes.begin(), scenes.end());
  return scenes.empty() ? fs::path() : scenes.front();
}
} // namespace

bool buildModuleHeadless(const Project& project, ModuleBuilder& builder)
{
  if (!builder.start(project))
    return false;
  std::optional<bool> built;
  while (!(built = builder.poll()))
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return *built;
}

int exportHeadless(const fs::path& projectFolder, const fs::path& outputFolder, bool development)
{
  const std::optional<Project> project = Project::open(projectFolder);
  if (!project)
    return 1;

  const BuildEnvironment environment = BuildEnvironment::current();
  ModuleBuilder builder(environment);
  if (!buildModuleHeadless(*project, builder))
    return 1;

  GameExporter::Options options;
  options.output = outputFolder.empty() ? project->exportFolder() / environment.platformName()
                                        : outputFolder;
  options.startScene = startScene(*project);
  options.module = builder.modulePath(*project);
  options.development = development;

  const char* base = SDL_GetBasePath();
  const GameExporter exporter(base ? fs::path(base) : fs::current_path());
  return exporter.run(*project, options).empty() ? 1 : 0;
}

int exportWebHeadless(const fs::path& projectFolder, const fs::path& outputFolder)
{
  const std::optional<Project> project = Project::open(projectFolder);
  if (!project)
    return 1;
  if (project->startScene().empty())
  {
    LOG_ERROR("The project has no start scene to play");
    return 1;
  }
  ModuleBuilder builder(BuildEnvironment::current());
  const fs::path output = outputFolder.empty() ? project->exportFolder() / "web" : outputFolder;
  if (!builder.startWeb(*project, output, 1280, 720))
    return 1;
  std::optional<bool> built;
  while (!(built = builder.poll()))
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return *built ? 0 : 1;
}

int fillPartsHeadless(const fs::path& projectFolder, std::string_view which, bool dryRun)
{
  const std::optional<Project> opened = Project::open(projectFolder);
  if (!opened)
    return 1;
  ModuleBuilder builder(BuildEnvironment::current());
  if (!buildModuleHeadless(*opened, builder))
    return 1;
  sfs::ModuleHost module;
  if (!module.load(builder.modulePath(*opened), opened->moduleName(), sfs::ModuleHost::Load::FromCopy))
    return 1;

  const FillPartsReport report = fillProjectParts(projectFolder, which, !dryRun);
  for (const FillPartsReport::ChangedPrefab& prefab : report.changed)
    LOG_INFO(prefab.path + ": " + std::to_string(prefab.fields) + " part fields " + (dryRun ? "to fill" : "filled"));
  LOG_INFO(std::to_string(report.prefabs) + " prefabs read; " + std::to_string(report.changed.size()) +
           " with part fields " +
           (dryRun ? "to fill (--dry-run: no prefab was written; the game module was built, into the project's "
                     ".sfs/build)"
                   : "filled"));
  return report.ok ? 0 : 1;
}
