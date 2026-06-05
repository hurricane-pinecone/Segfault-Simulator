#pragma once

#include <algorithm>
#include <cctype>
#include <string>

inline std::string toLower(std::string s)
{
  std::transform(s.begin(),
                 s.end(),
                 s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}
