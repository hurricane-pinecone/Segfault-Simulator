#include "engine/core/reflection/reflection.h"

#include <algorithm>
#include <mutex>

namespace sfs::reflect
{

void TypeRegistry::add(TypeInfo type)
{
  for (FieldInfo& field : type.fields)
    if (field.owner.empty())
      field.owner = type.name;
  const auto existing =
      std::find_if(m_types.begin(),
                   m_types.end(),
                   [&](const TypeInfo& t) { return t.name == type.name; });
  if (existing != m_types.end())
    *existing = std::move(type);
  else
    m_types.push_back(std::move(type));
  resolve();
}

void TypeRegistry::removeModule(std::string_view module)
{
  std::erase_if(
      m_types, [&](const TypeInfo& type) { return type.module == module; });
  resolve();
}

std::vector<std::string> TypeRegistry::chain(std::string_view name) const
{
  std::vector<std::string> out;
  for (const TypeInfo* type = find(name); type;
       type = type->base.empty() ? nullptr : find(type->base))
  {
    if (std::find(out.begin(), out.end(), type->name) != out.end())
      break; // (a cycle: never from C++, only from hand-made types)
    out.push_back(type->name);
  }
  return out;
}

bool TypeRegistry::isA(std::string_view type, std::string_view base) const
{
  const std::vector<std::string> names = chain(type);
  return std::find(names.begin(), names.end(), base) != names.end();
}

void TypeRegistry::resolve()
{
  inheritFields();
  m_referencing.clear();
  for (const TypeInfo& type : m_types)
    if (type.kind == TypeKind::Component &&
        std::any_of(type.fields.begin(), type.fields.end(), isEntityField))
      m_referencing.push_back(&type);
}

void TypeRegistry::inheritFields()
{
  // Nothing derives (the usual case): nothing to do.
  if (std::none_of(m_types.begin(),
                   m_types.end(),
                   [](const TypeInfo& type) { return !type.base.empty(); }))
    return;

  // Each type's own fields, by name, before anything is rebuilt.
  std::vector<std::vector<FieldInfo>> own(m_types.size());
  for (std::size_t i = 0; i < m_types.size(); ++i)
    for (const FieldInfo& field : m_types[i].fields)
      if (field.owner == m_types[i].name)
        own[i].push_back(field);
  const auto ownOf = [&](std::string_view name) -> const std::vector<FieldInfo>*
  {
    for (std::size_t i = 0; i < m_types.size(); ++i)
      if (m_types[i].name == name)
        return &own[i];
    return nullptr;
  };

  for (std::size_t i = 0; i < m_types.size(); ++i)
  {
    TypeInfo& type = m_types[i];
    if (type.base.empty())
      continue;
    // Root first, then each level down to the type itself. The address
    // functions of a base's fields work on a derived object as they are:
    // scriptable objects are handed round as their sfs::ScriptableObject
    // (see objectAs in registration.h).
    std::vector<std::string> levels = chain(type.name);
    std::vector<FieldInfo> fields;
    for (auto level = levels.rbegin(); level != levels.rend(); ++level)
      if (const std::vector<FieldInfo>* fieldsOf = ownOf(*level))
        fields.insert(fields.end(), fieldsOf->begin(), fieldsOf->end());
    type.fields = std::move(fields);
  }
}

const TypeInfo* TypeRegistry::find(std::string_view name) const
{
  const auto it =
      std::find_if(m_types.begin(),
                   m_types.end(),
                   [&](const TypeInfo& type) { return type.name == name; });
  return it != m_types.end() ? &*it : nullptr;
}

TypeRegistry& types()
{
  static TypeRegistry registry;
  static std::once_flag engineRegistered;
  std::call_once(engineRegistered, [] { registerEngineTypes(registry); });
  return registry;
}

} // namespace sfs::reflect
