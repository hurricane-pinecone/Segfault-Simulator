#include "engine/runtime/scripting/scriptRegistry.h"

#include "engine/core/components/scriptComponent.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <algorithm>

namespace sfs
{

void ScriptRegistry::add(ScriptType type)
{
  const auto existing =
      std::find_if(m_types.begin(),
                   m_types.end(),
                   [&](const ScriptType& t) { return t.name == type.name; });
  if (existing != m_types.end())
    *existing = std::move(type);
  else
    m_types.push_back(std::move(type));
}

void ScriptRegistry::removeModule(std::string_view module)
{
  std::erase_if(m_types,
                [&](const ScriptType& type) { return type.module == module; });
}

const ScriptType* ScriptRegistry::find(std::string_view name) const
{
  const auto it =
      std::find_if(m_types.begin(),
                   m_types.end(),
                   [&](const ScriptType& type) { return type.name == name; });
  return it != m_types.end() ? &*it : nullptr;
}

ScriptRegistry& scripts()
{
  static ScriptRegistry registry;
  return registry;
}

void startScripts(Scene& scene, const std::vector<std::string>& systems, MeshLibrary* meshes)
{
  const SceneSerializer serializer(reflect::types(), meshes);

  for (const std::string& name : systems)
  {
    const ScriptType* type = scripts().find(name);
    if (type && type->kind == ScriptType::Kind::System)
      type->addSystem(scene);
    else
      LOG_ERROR("Play: no game system named '" + name + "'");
  }

  // Entities first, then attach: onCreate may create more entities.
  std::vector<Entity> scripted;
  for (const Entity& entity : scene.entities())
    if (entity.hasComponent<ScriptComponent>() &&
        !entity.getComponent<ScriptComponent>().script.empty())
      scripted.push_back(entity);

  for (const Entity& entity : scripted)
  {
    const ScriptComponent& component = entity.getComponent<ScriptComponent>();
    const ScriptType* type = scripts().find(component.script);
    if (!type || type->kind != ScriptType::Kind::GameObject || !type->createObject)
    {
      LOG_ERROR("Play: no game object script named '" + component.script + "'");
      continue;
    }

    std::unique_ptr<GameObject> object = type->createObject();
    // This entity's field values, from the editor, before onCreate sees them.
    if (const reflect::TypeInfo* info = reflect::types().find(component.script))
      serializer.loadObject(*info, object.get(), component.fields);
    scene.adoptObject(std::move(object), entity);
  }
}

} // namespace sfs
