#include "engine/core/util/typeKey.h"

#include "typeIds.h"

#include <cstdlib>
#if defined(__GNUC__) || defined(__clang__)
  #include <cxxabi.h>
#endif

namespace sfs::detail
{

int TypeIds::idOf(std::string_view typeKey, bool* added)
{
  const std::lock_guard lock(m_mutex);
  const auto [it, inserted] =
      m_ids.try_emplace(std::string(typeKey), static_cast<int>(m_ids.size()));
  if (added)
    *added = inserted;
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
