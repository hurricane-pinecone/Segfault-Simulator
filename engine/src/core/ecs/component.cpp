#include "engine/core/ecs/component.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/core/util/fatalError.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#if defined(__GNUC__) || defined(__clang__)
  #include <cxxabi.h>
#endif

namespace sfs::detail
{

namespace
{

// Ids by type key, numbered from 0 in the order they're first asked for.
class TypeIds
{
public:
  // `added`, if given, says whether the id was assigned by this call.
  int idOf(std::string_view typeKey, bool* added = nullptr)
  {
    const std::lock_guard lock(m_mutex);
    const auto [it, inserted] =
        m_ids.try_emplace(std::string(typeKey), static_cast<int>(m_ids.size()));
    if (added)
      *added = inserted;
    return it->second;
  }

private:
  std::mutex m_mutex;
  std::unordered_map<std::string, int> m_ids;
};

} // namespace

int componentTypeId(std::string_view typeKey)
{
  static TypeIds ids;
  bool added = false;
  const int id = ids.idOf(typeKey, &added);

  // Development builds warn while there's still room to raise the limit.
  constexpr int kHeadroom = 8;
  const int limit = static_cast<int>(MAX_COMPONENTS);
  if (added && debugFeatures::compiled() && id >= limit - kHeadroom &&
      id < limit)
    LOG_WARN("Component type " + readableTypeName(typeKey) + " is number " +
             std::to_string(id + 1) + " of MAX_COMPONENTS (" +
             std::to_string(limit) + "); raise MAX_COMPONENTS in entity.h");

  // The id is a bit index into a fixed-width Signature; an id at or past the
  // ceiling has no bit to represent it. Stop here, where the type is known,
  // rather than with an opaque out-of-range deep in the registry. Every time:
  // the caller's cached id isn't set when this throws, so it asks again.
  if (id >= static_cast<int>(MAX_COMPONENTS))
    fatalError("Too many component types: " + readableTypeName(typeKey) +
               " would be number " + std::to_string(id + 1) +
               ", past MAX_COMPONENTS (" + std::to_string(MAX_COMPONENTS) +
               "); raise MAX_COMPONENTS in entity.h");

  return id;
}

int systemTypeId(std::string_view typeKey)
{
  static TypeIds ids;
  return ids.idOf(typeKey);
}

std::string readableTypeName(std::string_view typeKey)
{
#if defined(_MSC_VER) && !defined(__clang__)
  // "... componentTypeKey<struct farm::Seed>(void)"
  constexpr std::string_view open = "componentTypeKey<";
  const std::size_t start = typeKey.find(open);
  const std::size_t end = typeKey.rfind(">(");
  if (start == std::string_view::npos || end == std::string_view::npos ||
      end <= start)
    return std::string(typeKey);
  std::string_view name =
      typeKey.substr(start + open.size(), end - (start + open.size()));
  for (const std::string_view prefix : {"struct ", "class ", "enum "})
    if (name.starts_with(prefix))
      name.remove_prefix(prefix.size());
  return std::string(name);
#else
  // "... componentTypeKey() [with T = farm::Seed; ...]" (GCC) or
  // "... componentTypeKey() [T = farm::Seed]" (Clang)
  constexpr std::string_view open = "T = ";
  const std::size_t start = typeKey.find(open);
  if (start == std::string_view::npos)
    return std::string(typeKey);
  const std::size_t end = typeKey.find_first_of(";]", start);
  return std::string(typeKey.substr(start + open.size(),
                                    end == std::string_view::npos
                                        ? std::string_view::npos
                                        : end - (start + open.size())));
#endif
}

std::string readableTypeName(const std::type_info& type)
{
  std::string name = type.name();
#if defined(__GNUC__) || defined(__clang__)
  int status = 0;
  char* demangled =
      abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
  if (status == 0 && demangled)
    name = demangled;
  std::free(demangled);
#endif
  return name;
}

} // namespace sfs::detail
