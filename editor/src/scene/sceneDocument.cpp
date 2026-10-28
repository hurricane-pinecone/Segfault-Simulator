#include "scene/sceneDocument.h"

#include "scene/editorScene.h"

#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/serialization/sceneSerializer.h"

namespace
{
sfs::SceneSerializer serializerFor(EditorScene& scene)
{
  return scene.serializer();
}
} // namespace

void SceneDocument::newScene()
{
  m_opening.reset();
  m_scene->resetToDefault();
  m_path.clear();
}

bool SceneDocument::open(const std::filesystem::path& path)
{
  if (!beginOpen(path))
    return false;
  finishOpening();
  return true;
}

bool SceneDocument::beginOpen(const std::filesystem::path& path)
{
  m_opening.reset();
  m_scene->clear();
  std::optional<nlohmann::json> file = sfs::SceneSerializer::readJsonFile(path);
  // The settings (and an empty entity list) now; the entities a few at a time.
  nlohmann::json entities = file && file->contains("entities") ? std::move((*file)["entities"]) : nlohmann::json();
  if (file)
    (*file)["entities"] = nlohmann::json::array();
  if (!file || !entities.is_array() || !serializerFor(*m_scene).load(*file, *m_scene, &m_scene->gameSystems()))
  {
    if (file)
      LOG_ERROR("Can't open the scene " + path.string());
    // Don't leave a half-loaded scene pretending to be that file.
    m_scene->resetToDefault();
    m_path.clear();
    return false;
  }
  m_path = path;
  m_opening = Opening{std::move(entities)};
  return true;
}

bool SceneDocument::continueOpening(std::chrono::milliseconds budget)
{
  if (!m_opening)
    return true;
  const auto until = std::chrono::steady_clock::now() + budget;
  const sfs::SceneSerializer serializer = serializerFor(*m_scene);
  do
  {
    if (m_opening->next >= m_opening->entities.size())
    {
      m_opening.reset();
      m_scene->flushEntities();
      LOG_INFO("Opened scene " + m_path.filename().string());
      return true;
    }
    const nlohmann::json& entry = m_opening->entities[m_opening->next++];
    if (entry.is_object())
      serializer.loadEntity(entry, *m_scene);
  } while (std::chrono::steady_clock::now() < until);
  m_scene->flushEntities();
  return false;
}

void SceneDocument::finishOpening()
{
  while (!continueOpening(std::chrono::milliseconds(1000)))
  {
  }
}

std::pair<std::size_t, std::size_t> SceneDocument::openingProgress() const
{
  return m_opening ? std::make_pair(m_opening->next, m_opening->entities.size()) : std::make_pair<std::size_t, std::size_t>(0, 0);
}

bool SceneDocument::save()
{
  if (m_path.empty())
    return false;
  finishOpening(); // all of it, not what's in so far

  if (!serializerFor(*m_scene).saveFile(*m_scene, m_path, m_scene->gameSystems()))
    return false;
  LOG_INFO("Saved scene " + m_path.filename().string());
  return true;
}

bool SceneDocument::saveAs(const std::filesystem::path& path)
{
  const std::filesystem::path previous = m_path;
  m_path = path;
  if (save())
    return true;
  m_path = previous;
  return false;
}

nlohmann::json SceneDocument::snapshot()
{
  finishOpening();
  return serializerFor(*m_scene).save(*m_scene, m_scene->gameSystems());
}

void SceneDocument::restore(const nlohmann::json& snapshot)
{
  m_opening.reset();
  m_scene->clear();
  serializerFor(*m_scene).load(snapshot, *m_scene, &m_scene->gameSystems());
  m_scene->flushEntities();
}
