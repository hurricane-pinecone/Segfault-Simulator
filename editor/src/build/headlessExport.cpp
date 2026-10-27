#include "build/headlessExport.h"

#include "build/buildEnvironment.h"
#include "build/gameExporter.h"
#include "build/moduleBuilder.h"
#include "project/project.h"
#include "scene/sceneDocument.h"

#include "engine/core/logger/logger.h"

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

int exportHeadless(const fs::path& projectFolder, const fs::path& outputFolder)
{
  const std::optional<Project> project = Project::open(projectFolder);
  if (!project)
    return 1;

  const BuildEnvironment environment = BuildEnvironment::current();
  ModuleBuilder builder(environment);
  if (!builder.start(*project))
    return 1;
  std::optional<bool> built;
  while (!(built = builder.poll()))
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (!*built)
    return 1;

  GameExporter::Options options;
  options.output = outputFolder.empty() ? project->exportFolder() / environment.platformName()
                                        : outputFolder;
  options.startScene = startScene(*project);
  options.module = builder.modulePath(*project);

  const char* base = SDL_GetBasePath();
  const GameExporter exporter(base ? fs::path(base) : fs::current_path());
  return exporter.run(*project, options).empty() ? 1 : 0;
}
