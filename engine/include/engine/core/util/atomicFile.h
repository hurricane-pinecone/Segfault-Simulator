#pragma once

#include <filesystem>
#include <string_view>

namespace sfs
{

/**
 * Replace a file's contents so that a crash or power cut at any moment leaves
 * either the old file or the new one, never a half-written mix: the contents
 * go to a temporary file beside it (".<name>.tmp-..." in the same folder),
 * which is flushed to disk and then renamed over the file. On Windows the
 * rename is MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
 * retried briefly while another program (a virus scanner, an indexer) holds
 * the file.
 *
 * Every file the editor and the engine write (scenes, prefabs, materials,
 * .meta files, project files, scripts) goes through here.
 *
 * The folder must exist. A failure is logged, and leaves the file as it was.
 *
 * @return true once the new contents are in place
 */
bool writeFileAtomically(const std::filesystem::path& path,
                         std::string_view contents);

} // namespace sfs
