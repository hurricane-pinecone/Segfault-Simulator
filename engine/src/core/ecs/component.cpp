#include "engine/core/ecs/component.h"

#include "engine/core/util/fatalError.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace sfs::detail
{

int componentTypeId(std::string_view typeKey)
{
  static std::mutex mutex;
  static std::unordered_map<std::string, int> ids;

  const std::lock_guard lock(mutex);

  const auto [it, inserted] =
      ids.try_emplace(std::string(typeKey), static_cast<int>(ids.size()));

  // The id is a bit index into a fixed-width Signature; an id at or past the
  // ceiling has no bit to represent it. Stop here, where the type is known,
  // rather than with an opaque out-of-range deep in the registry. Every time:
  // the caller's cached id isn't set when this throws, so it asks again.
  if (it->second >= static_cast<int>(MAX_COMPONENTS))
    fatalError("Too many component types: " + readableTypeName(typeKey) +
               " would be number " + std::to_string(it->second + 1) +
               ", past MAX_COMPONENTS (" + std::to_string(MAX_COMPONENTS) +
               "); raise MAX_COMPONENTS in entity.h");

  return it->second;
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

} // namespace sfs::detail
