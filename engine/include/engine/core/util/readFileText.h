#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sfs
{

/**
 * A file's whole contents, byte for byte.
 *
 * @param path the file
 * @return its contents, or nothing if it can't be opened
 */
std::optional<std::string> readFileText(const std::filesystem::path& path);

} // namespace sfs
