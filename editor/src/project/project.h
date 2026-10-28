#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

// A game project on disk: a folder holding `project.sfsproj` (JSON metadata)
// and the project's content folders (scenes, assets, scripts).
class Project
{
public:
  static constexpr const char* kFileName = "project.sfsproj";

  /**
   * Open the project in `folder`.
   *
   * @return the project, or nothing (logged) if the folder has no readable
   * project file.
   */
  static std::optional<Project> open(const std::filesystem::path& folder);

  /**
   * Create a new project named `name` in `folder` (created if missing): writes
   * the project file and the standard content folders.
   *
   * @return the project, or nothing (logged) if the folder already holds a
   * project or can't be written.
   */
  static std::optional<Project> create(const std::filesystem::path& folder,
                                       const std::string& name);

  const std::filesystem::path& root() const { return m_root; }
  const std::string& name() const { return m_name; }

  std::filesystem::path scenesFolder() const { return m_root / "scenes"; }
  std::filesystem::path assetsFolder() const { return m_root / "assets"; }
  std::filesystem::path scriptsFolder() const { return m_root / "scripts"; }
  std::filesystem::path prefabsFolder() const { return m_root / "prefabs"; }

  /**
   * The scene an exported game starts in (relative to the project folder), or
   * empty when none has been chosen.
   */
  const std::filesystem::path& startScene() const { return m_startScene; }

  /** Choose the start scene (any path inside the project) and save it. */
  bool setStartScene(const std::filesystem::path& scene);

  /** Where Export puts games by default (ignored by version control). */
  std::filesystem::path exportFolder() const { return m_root / "builds"; }

  /** Where the editor builds the game module (ignored by version control). */
  std::filesystem::path buildFolder() const { return m_root / ".sfs" / "build"; }

  /**
   * The game module's name: the project name as a C++/CMake identifier. Also
   * the module recorded on its registered types.
   */
  std::string moduleName() const;

  /**
   * Write the project's CMakeLists.txt (building its scripts into a game
   * module) and .gitignore, unless they already exist -- older projects gain
   * them on their first build.
   *
   * @return false (logged) if a file can't be written.
   */
  bool ensureBuildFiles() const;

private:
  Project(std::filesystem::path root, std::string name)
      : m_root(std::move(root)), m_name(std::move(name))
  {
  }

  std::filesystem::path m_root;
  std::string m_name;
  std::filesystem::path m_startScene;
};
