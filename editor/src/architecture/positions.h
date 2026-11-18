#pragma once

#include <imgui.h>

#include <filesystem>
#include <string>
#include <unordered_map>

// Where the Architecture board's boxes are, kept per project in
// .sfs/architecture.json so the board looks the same each time it's opened.
namespace architecture
{

/** Each box's top left corner, in canvas units, by its node's id. */
using Positions = std::unordered_map<std::string, ImVec2>;

/**
 * Read saved positions. A missing file is no positions; a file that can't be
 * read is logged and taken as none.
 *
 * @param file the file
 * @return the positions it holds
 */
Positions readPositions(const std::filesystem::path& file);

/**
 * Save positions, one box to a line sorted by id (to a tenth of a canvas
 * unit), creating the file's folder if it's missing.
 *
 * @param file the file
 * @param positions what to save
 * @return false (logged) if it couldn't be written
 */
bool writePositions(const std::filesystem::path& file,
                    const Positions& positions);

} // namespace architecture
