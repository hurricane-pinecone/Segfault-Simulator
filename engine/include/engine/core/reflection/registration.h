#pragma once

#include "engine/core/assets/scriptableObject.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/reflection/reflection.h"

#include <string>
#include <utility>

// Helpers used by sfs-codegen's generated registration code. Not intended for
// hand-written use: the generator emits these calls from your declarations.
namespace sfs::reflect
{

/** Type-erased component operations for T. */
template <typename T>
ComponentOps componentOps()
{
  ComponentOps ops;
  ops.has = [](const Entity& entity) { return entity.hasComponent<T>(); };
  ops.get = [](const Entity& entity) -> void*
  { return entity.hasComponent<T>() ? &entity.getComponent<T>() : nullptr; };
  ops.add = [](const Entity& entity) { Entity(entity).addComponent<T>(); };
  ops.remove = [](const Entity& entity) { entity.removeComponent<T>(); };
  return ops;
}

/** A TypeInfo for component T, ready for fields to be appended. */
template <typename T>
TypeInfo
componentType(std::string name, std::string displayName, std::string module)
{
  TypeInfo type;
  type.name = std::move(name);
  type.displayName = std::move(displayName);
  type.module = std::move(module);
  type.kind = TypeKind::Component;
  type.component = componentOps<T>();
  return type;
}

/** A TypeInfo for scriptable object type T (an sfs::ScriptableObject), ready
 * for fields to be appended. */
template <typename T>
TypeInfo scriptableObjectType(std::string name,
                              std::string displayName,
                              std::string module)
{
  TypeInfo type;
  type.name = std::move(name);
  type.displayName = std::move(displayName);
  type.module = std::move(module);
  type.kind = TypeKind::ScriptableObject;
  type.scriptable.create = []() -> void*
  { return static_cast<sfs::ScriptableObject*>(new T()); };
  type.scriptable.destroy = [](void* object)
  { delete static_cast<sfs::ScriptableObject*>(object); };
  return type;
}

/**
 * Access to a member regardless of its access level, for [[sfs::show]] on a
 * private field. An explicit instantiation may name a private member (the
 * standard exempts it from access checking); instantiating MemberAccess there
 * defines the tag's friend accessor, which hands the member pointer back:
 *
 *   struct Tag { using type = int Player::*; friend type memberOf(Tag); };
 *   template struct MemberAccess<Tag, &Player::hitPoints>;
 *   int Player::* pointer = memberOf(Tag{});
 */
template <typename Tag, typename Tag::type Member>
struct MemberAccess
{
  friend typename Tag::type memberOf(Tag) { return Member; }
};

} // namespace sfs::reflect
