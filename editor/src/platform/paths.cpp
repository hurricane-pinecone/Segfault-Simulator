#include "platform/paths.h"

#include <algorithm>
#include <vector>

namespace paths
{

std::string normal(const std::filesystem::path& path)
{
  // Not generic_string(): it can fold a share's leading "//" into "/".
  std::string text = path.string();
#if defined(_WIN32)
  std::replace(text.begin(), text.end(), '\\', '/');
#endif
  std::string prefix = text.starts_with("//") ? "//" : text.starts_with("/") ? "/" : "";
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size())
  {
    const std::size_t end = std::min(text.find('/', start), text.size());
    const std::string part = text.substr(start, end - start);
    if (part == "..")
    {
      if (!parts.empty() && parts.back() != "..")
        parts.pop_back();
      else if (prefix.empty())
        parts.push_back(part);
    }
    else if (!part.empty() && part != ".")
      parts.push_back(part);
    start = end + 1;
  }
  std::string out = prefix;
  for (std::size_t i = 0; i < parts.size(); ++i)
    out += (i > 0 ? "/" : "") + parts[i];
  return out;
}

std::optional<std::string> relative(const std::filesystem::path& path, const std::filesystem::path& folder)
{
  const std::string p = normal(path), f = normal(folder);
  if (p == f)
    return std::string();
  if (p.starts_with(f + "/"))
    return p.substr(f.size() + 1);
  return std::nullopt;
}

bool within(const std::filesystem::path& path, const std::filesystem::path& folder)
{
  return paths::relative(path, folder).has_value();
}

std::filesystem::path moved(const std::filesystem::path& path, const std::filesystem::path& from,
                            const std::filesystem::path& to)
{
  const std::optional<std::string> rest = paths::relative(path, from);
  if (!rest)
    return path;
  return rest->empty() ? to : std::filesystem::path(normal(to) + "/" + *rest);
}

} // namespace paths
