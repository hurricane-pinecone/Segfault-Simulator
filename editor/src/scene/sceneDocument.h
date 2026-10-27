#pragma once

#include "nlohmann/json.hpp"

#include <filesystem>

class EditorScene;

// The scene file being edited: tracks its path and moves the EditorScene's
// contents to and from disk (and memory, for Play/Stop and module reloads)
// through the reflection-driven SceneSerializer.
class SceneDocument
{
public:
  static constexpr const char* kExtension = ".scene";

  explicit SceneDocument(EditorScene& scene) : m_scene(&scene) {}

  /** Point at a recreated scene (after a module reload), keeping the path. */
  void rebind(EditorScene& scene) { m_scene = &scene; }

  /** The file this scene saves to; empty for a new, unsaved scene. */
  const std::filesystem::path& path() const { return m_path; }
  bool hasPath() const { return !m_path.empty(); }

  /** Replace the scene with the default layout, not yet saved anywhere. */
  void newScene();

  /** Replace the scene with a file's contents. @return false (logged) on failure. */
  bool open(const std::filesystem::path& path);

  /** Save to the current path. @return false if there is none or writing fails. */
  bool save();

  /** Save to `path`, which becomes the current path. */
  bool saveAs(const std::filesystem::path& path);

  /** The scene's current contents, in scene-file form. */
  nlohmann::json snapshot() const;

  /** Replace the scene's contents with a snapshot. */
  void restore(const nlohmann::json& snapshot);

private:
  EditorScene* m_scene;
  std::filesystem::path m_path;
};
