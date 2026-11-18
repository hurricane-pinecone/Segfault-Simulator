#include "architecture/positions.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/atomicFile.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace architecture
{

namespace
{

// The file's own version: it goes up if its contents change meaning, and
// older layouts are laid out afresh. Version 3 put each kind of node in a
// column of its own. The file keeps every box, moved or not, so it can't
// tell the hand-moved ones apart to keep them.
constexpr int FILE_VERSION = 3;

std::string number(float value);

} // namespace

Positions readPositions(const std::filesystem::path& file)
{
  Positions positions;
  std::error_code error;

  if (!std::filesystem::exists(file, error))
    return positions;

  std::ifstream in(file, std::ios::binary);
  const std::string text(std::istreambuf_iterator<char>(in), {});
  const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
  const auto saved = root.is_object() ? root.find("positions") : root.end();

  if (root.is_discarded() || !root.is_object() || saved == root.end() ||
      !saved->is_object())
  {
    LOG_WARN("The Architecture board's saved layout can't be read, so it's "
             "laid out afresh: " +
             file.string());
    return positions;
  }

  const auto version = root.find("version");

  if (version == root.end() || !version->is_number_integer() ||
      version->get<int>() < FILE_VERSION)
  {
    LOG_INFO("The Architecture board's layout has changed since its boxes "
             "were saved, so it's laid out afresh: " +
             file.string());
    return positions;
  }

  for (const auto& [id, at] : saved->items())
  {
    if (!at.is_array() || at.size() != 2 || !at[0].is_number() ||
        !at[1].is_number())
      continue;

    positions.emplace(id, ImVec2(at[0].get<float>(), at[1].get<float>()));
  }

  return positions;
}

bool writePositions(const std::filesystem::path& file,
                    const Positions& positions)
{
  std::error_code error;
  std::filesystem::create_directories(file.parent_path(), error);

  // One box to a line, sorted by id: the same layout writes the same file.
  std::vector<const Positions::value_type*> sorted;
  sorted.reserve(positions.size());

  for (const Positions::value_type& entry : positions)
    sorted.push_back(&entry);

  std::sort(sorted.begin(),
            sorted.end(),
            [](const auto* a, const auto* b) { return a->first < b->first; });
  std::string text =
      "{\"version\": " + std::to_string(FILE_VERSION) + ", \"positions\": {";

  for (std::size_t i = 0; i < sorted.size(); ++i)
  {
    const auto& [id, at] = *sorted[i];
    text += i == 0 ? "\n  " : ",\n  ";
    text += nlohmann::json(id).dump() + ": [" + number(at.x) + ", " +
            number(at.y) + "]";
  }

  text += "\n}}\n";
  return sfs::writeFileAtomically(file, text);
}

namespace
{

// A position's coordinate to a tenth, whatever the locale.
std::string number(float value)
{
  char text[32];
  const auto [end, error] = std::to_chars(
      text, text + sizeof(text), value, std::chars_format::fixed, 1);
  return error == std::errc() ? std::string(text, end) : std::string("0");
}

} // namespace

} // namespace architecture
