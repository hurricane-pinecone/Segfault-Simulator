#pragma once

#include "nlohmann/json.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <utility>

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

  /** Change the path it saves to (e.g. back to the scene after editing a prefab). */
  void setPath(const std::filesystem::path& path) { m_path = path; }

  /** Replace the scene with the default layout, not yet saved anywhere. */
  void newScene();

  /** Replace the scene with a file's contents. @return false (logged) on failure. */
  bool open(const std::filesystem::path& path);

  /**
   * Start replacing the scene with a file's contents: its settings now, its
   * entities a few at a time (continueOpening), so the editor stays drawn
   * while a big scene comes in. Anything needing the whole scene (snapshot,
   * save) finishes it first. @return false (logged) if it can't be read.
   */
  bool beginOpen(const std::filesystem::path& path);

  /** Add more of the scene's entities, for up to `budget`. @return true once it's all in. */
  bool continueOpening(std::chrono::milliseconds budget);

  /** Add the rest at once. */
  void finishOpening();

  bool opening() const { return m_opening.has_value(); }
  /** Entities in so far, of how many (while opening). */
  std::pair<std::size_t, std::size_t> openingProgress() const;

  /** Save to the current path. @return false if there is none or writing fails. */
  bool save();

  /** Save to `path`, which becomes the current path. */
  bool saveAs(const std::filesystem::path& path);

  /** The scene's current contents, in scene-file form (finishing opening it first). */
  nlohmann::json snapshot();

  /** Replace the scene's contents with a snapshot. */
  void restore(const nlohmann::json& snapshot);

private:
  EditorScene* m_scene;
  std::filesystem::path m_path;

  // A file being opened: its entities still to add.
  struct Opening
  {
    nlohmann::json entities;
    std::size_t next = 0;
  };
  std::optional<Opening> m_opening;
};
