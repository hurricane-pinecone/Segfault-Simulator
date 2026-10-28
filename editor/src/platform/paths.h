#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Path arithmetic that works on network shares (a project in WSL, opened as
// \\wsl.localhost\...): MinGW's lexically_normal and lexically_relative
// mangle UNC paths, so these work on forward-slash strings instead.
namespace paths
{

/** `path` with forward slashes, "." and ".." folded, and a share's leading "//" kept. */
std::string normal(const std::filesystem::path& path);

/** Whether `path` is `folder` or inside it. */
bool within(const std::filesystem::path& path, const std::filesystem::path& folder);

/** `path` relative to `folder` ("" if it's the folder itself); nothing if it isn't inside. */
std::optional<std::string> relative(const std::filesystem::path& path, const std::filesystem::path& folder);

/** Where `path` is after `from` moved to `to` (itself if it wasn't in `from`). */
std::filesystem::path moved(const std::filesystem::path& path, const std::filesystem::path& from,
                            const std::filesystem::path& to);

} // namespace paths
