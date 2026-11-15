#pragma once

#include <string>
#include <string_view>
#include <typeinfo>

// Names and ids for C++ types that every module agrees on: the engine, an
// editor and a game module loaded at runtime. Component, system and event ids
// all come from here, each from a table of their own.
namespace sfs::detail
{

/**
 * A name for T that is identical in every module built by the same compiler:
 * the compiler's own spelling of this function's signature, which embeds T.
 */
template <typename T>
constexpr std::string_view componentTypeKey()
{
#if defined(_MSC_VER) && !defined(__clang__)
  return __FUNCSIG__;
#else
  return __PRETTY_FUNCTION__;
#endif
}

/**
 * The type a type key names, for people ("Health", "farm::Seed"); the whole
 * key if it can't be picked out.
 */
std::string readableTypeName(std::string_view typeKey);

/**
 * A run-time type's name, for people: demangled where the compiler mangles
 * it ("farm::HarvestSystem"), else as the compiler gives it.
 * @param type the type, e.g. typeid(*this) for an object's own type
 * @return its name
 */
std::string readableTypeName(const std::type_info& type);

/**
 * T's id from `idOf` (componentTypeId, systemTypeId or eventTypeId), asked for
 * the first time and then remembered in the calling module. The table behind
 * `idOf` is shared, so every module gets the same id. If `idOf` raises a fatal
 * error, nothing is remembered and the next call asks again.
 * @return T's id
 */
template <typename T, int (*idOf)(std::string_view)>
int moduleTypeId()
{
  static const int id = idOf(componentTypeKey<T>());
  return id;
}

} // namespace sfs::detail
