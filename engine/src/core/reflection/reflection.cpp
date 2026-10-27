#include "engine/core/reflection/reflection.h"

#include <algorithm>
#include <mutex>

namespace sfs::reflect
{

void TypeRegistry::add(TypeInfo type)
{
  const auto existing =
      std::find_if(m_types.begin(),
                   m_types.end(),
                   [&](const TypeInfo& t) { return t.name == type.name; });
  if (existing != m_types.end())
    *existing = std::move(type);
  else
    m_types.push_back(std::move(type));
}

void TypeRegistry::removeModule(std::string_view module)
{
  std::erase_if(m_types,
                [&](const TypeInfo& type) { return type.module == module; });
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
