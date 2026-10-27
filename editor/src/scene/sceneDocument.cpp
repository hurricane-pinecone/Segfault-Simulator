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
  return sfs::SceneSerializer(sfs::reflect::types(), &scene.renderSystem().meshes());
}
} // namespace

void SceneDocument::newScene()
{
  m_scene->resetToDefault();
  m_path.clear();
}

bool SceneDocument::open(const std::filesystem::path& path)
{
  m_scene->clear();
  if (!serializerFor(*m_scene).loadFile(path, *m_scene, &m_scene->gameSystems()))
  {
    // Don't leave a half-loaded scene pretending to be that file.
    m_scene->resetToDefault();
    m_path.clear();
    return false;
  }

  m_scene->flushEntities();
  m_path = path;
  LOG_INFO("Opened scene " + path.filename().string());
  return true;
}

bool SceneDocument::save()
{
  if (m_path.empty())
    return false;

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

nlohmann::json SceneDocument::snapshot() const
{
  return serializerFor(*m_scene).save(*m_scene, m_scene->gameSystems());
}

void SceneDocument::restore(const nlohmann::json& snapshot)
{
  m_scene->clear();
  serializerFor(*m_scene).load(snapshot, *m_scene, &m_scene->gameSystems());
  m_scene->flushEntities();
}
