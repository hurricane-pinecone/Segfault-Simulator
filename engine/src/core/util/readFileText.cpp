#include "engine/core/util/readFileText.h"

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>

namespace sfs
{

std::optional<std::string> readFileText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);

  if (!in)
    return std::nullopt;

  return std::string(
      std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace sfs
