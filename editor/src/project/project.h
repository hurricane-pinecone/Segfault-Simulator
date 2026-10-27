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

private:
  Project(std::filesystem::path root, std::string name)
      : m_root(std::move(root)), m_name(std::move(name))
  {
  }

  std::filesystem::path m_root;
  std::string m_name;
};
