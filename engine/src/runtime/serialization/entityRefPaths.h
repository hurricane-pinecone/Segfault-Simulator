#pragma once

// Entity reference paths, shared by SceneSerializer's halves (the forms are
// described in sceneSerializer.h).

#include "nlohmann/json.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sfs::entityRefs
{

/**
 * Whether a saved reference is a path relative to its context root (not
 * scene-absolute "/7", nor a saveEntity() branch reference "#3").
 */
inline bool isRelative(std::string_view value)
{
  return value.empty() || (value.front() != '/' && value.front() != '#');
}

/**
 * Call `visit` with each non-empty segment of a "/"-separated path, in order,
 * until it returns false.
 *
 * @return false if `visit` stopped the walk
 */
template <typename Visit>
bool forEachSegment(std::string_view path, Visit&& visit)
{
  for (std::size_t start = 0; start < path.size();)
  {
    const std::size_t slash = std::min(path.find('/', start), path.size());
    const std::string_view segment = path.substr(start, slash - start);
    start = slash + 1;
    if (!segment.empty() && !visit(segment))
      return false;
  }
  return true;
}

/**
 * A path segment as an id ("101", "2318043377").
 *
 * @return the id, or nothing if the segment isn't one
 */
inline std::optional<uint32_t> segmentId(std::string_view segment)
{
  uint32_t id = 0;
  const auto [end, error] =
      std::from_chars(segment.data(), segment.data() + segment.size(), id);
  if (segment.empty() || error != std::errc() ||
      end != segment.data() + segment.size())
    return std::nullopt;
  return id;
}

/**
 * A relative path followed from a key: its segments appended to `base`'s,
 * each ".." dropping the last one first. A ".." past the root names nothing:
 * then `base` and `path` are joined as written, so the value is kept as it
 * was and saved back so.
 *
 * @param base the key the path is relative to ("" the root; "5/1")
 * @param path the relative path ("", "101", "../3")
 * @return the key it names ("5/1/101"), or the two joined as written
 */
std::string followPath(std::string_view base, std::string_view path);

/**
 * The relative path from one key to another (the inverse of followPath()).
 *
 * @param to   the key named ("5/1/23")
 * @param from the key it is written relative to ("5")
 * @return the path ("1/23"; ".." for each of `from`'s segments not shared)
 */
std::string relativePath(std::string_view to, std::string_view from);

/**
 * An entry's prefab "$id" (PrefabLinkComponent::id).
 *
 * @return it, or nothing if the entry has none (or not an integer from 0 to
 *         UINT32_MAX)
 */
inline std::optional<uint32_t> idOf(const nlohmann::json& entry)
{
  const auto it = entry.find("$id");
  if (it == entry.end() || !it->is_number_integer())
    return std::nullopt;
  const int64_t id = it->get<int64_t>();
  if (id < 0 || id > int64_t{UINT32_MAX})
    return std::nullopt;
  return static_cast<uint32_t>(id);
}

/** The source pending references from a scene's own entries name. */
inline constexpr std::string_view kSceneSource = "Scene";

/** The key an entry's scene id (SceneIdComponent) is saved under. */
inline constexpr const char* kSceneIdKey = "$sceneId";

/**
 * An entry's "$sceneId".
 *
 * @return it, or nothing if the entry has none (or not an integer from 1 to
 *         UINT32_MAX)
 */
inline std::optional<uint32_t> sceneIdOf(const nlohmann::json& entry)
{
  const auto it = entry.find(kSceneIdKey);
  if (it == entry.end() || !it->is_number_integer())
    return std::nullopt;
  const int64_t id = it->get<int64_t>();
  if (id < 1 || id > int64_t{UINT32_MAX})
    return std::nullopt;
  return static_cast<uint32_t>(id);
}

} // namespace sfs::entityRefs
