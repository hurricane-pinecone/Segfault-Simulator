#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

inline std::string toLower(std::string s)
{
  std::transform(s.begin(),
                 s.end(),
                 s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

namespace sfs
{

/**
 * Whether `text` matches a glob `pattern`: `*` matches any run of characters
 * (none included), `?` any one character, anything else itself.
 *
 * @param text    the text to test, e.g. "blade-2"
 * @param pattern the pattern, e.g. "blade-*"
 * @return true if the whole text matches the whole pattern
 */
inline bool globMatch(std::string_view text, std::string_view pattern)
{
  std::size_t t = 0, p = 0;
  // Where the last `*` was, and the text it's matched up to so far.
  std::size_t star = std::string_view::npos, starText = 0;
  while (t < text.size())
  {
    // A `*` in the pattern is always a wildcard, even against a `*` in the
    // text.
    if (p < pattern.size() && pattern[p] == '*')
    {
      star = p++;
      starText = t;
    }
    else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
    {
      ++t;
      ++p;
    }
    else if (star != std::string_view::npos)
    {
      p = star + 1;
      t = ++starText;
    }
    else
      return false;
  }
  while (p < pattern.size() && pattern[p] == '*')
    ++p;
  return p == pattern.size();
}

/**
 * A model node's name without the numeric suffix Blender gives a copy
 * ("link-lower.001" -> "link-lower"), as node names are matched.
 *
 * @param name the node's name
 * @return the name without a trailing ".<digits>", if it has one
 */
inline std::string_view baseNodeName(std::string_view name)
{
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot + 1 >= name.size())
    return name;
  for (std::size_t i = dot + 1; i < name.size(); ++i)
    if (name[i] < '0' || name[i] > '9')
      return name;
  return name.substr(0, dot);
}

/**
 * Whether a model node's name is the part a [[sfs::part]] pattern names: the
 * name itself, or without Blender's copy suffix (baseNodeName()), matches the
 * glob.
 *
 * @param name    the node's name, e.g. "blade-2.001"
 * @param pattern the part's name or glob, e.g. "blade-*"
 * @return true if it matches
 */
inline bool matchesPart(std::string_view name, std::string_view pattern)
{
  return globMatch(name, pattern) || globMatch(baseNodeName(name), pattern);
}

} // namespace sfs
