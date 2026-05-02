#pragma once

#include <cstdio>

inline const char* formatBytes(std::size_t bytes)
{
  static thread_local char buffer[64];

  const double kb = 1024.0;
  const double mb = kb * 1024.0;
  const double gb = mb * 1024.0;

  if (bytes >= gb)
  {
    std::snprintf(buffer, sizeof(buffer), "%.2f GB", bytes / gb);
  }
  else if (bytes >= mb)
  {
    std::snprintf(buffer, sizeof(buffer), "%.2f MB", bytes / mb);
  }
  else if (bytes >= kb)
  {
    std::snprintf(buffer, sizeof(buffer), "%.2f KB", bytes / kb);
  }
  else
  {
    std::snprintf(buffer, sizeof(buffer), "%zu B", bytes);
  }

  return buffer;
}
