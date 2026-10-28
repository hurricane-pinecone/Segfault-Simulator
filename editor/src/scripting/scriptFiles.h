#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Finding scripts' files. A script type's file is named after its class (the
// code generator requires it), anywhere in the scripts folder.
namespace scriptFiles
{

enum class Prefer
{
  Source, // its .cpp (the behaviour), else its header
  Header, // its .h (the declaration), else its source
};

/**
 * The file declaring or defining class `name` under `scriptsFolder`; nothing
 * if there's none. Searched when asked (the folder may be on a slow share).
 */
std::optional<std::filesystem::path> find(const std::filesystem::path& scriptsFolder, std::string_view name,
                                          Prefer prefer);

/**
 * Move or rename files and folders inside the scripts folder (`moves`: from ->
 * to, absolute paths), keeping the scripts building: every #include that
 * pointed at something moved is rewritten (from the scripts folder, the
 * include root) where it no longer finds it.
 *
 * @return false (logged) if one can't be moved: a target is taken, or the
 * file system refuses (moves before it are kept)
 */
bool move(const std::filesystem::path& scriptsFolder,
          const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& moves);

/**
 * Copy a script or a folder of them into `folder` (a script with its .h/.cpp
 * pair). Class names must be unique, so every class copied is renamed
 * <Name>Copy (<Name>Copy2...), its files with it, and the copies' includes of
 * each other point at the copies. A folder whose name is taken there becomes
 * "<Name> Copy".
 *
 * @return what was made (the files, or the folder); empty (logged) if it failed
 */
std::vector<std::filesystem::path> copy(const std::filesystem::path& scriptsFolder,
                                        const std::filesystem::path& item,
                                        const std::filesystem::path& folder);

/**
 * Rename identifier `from` to `to` in every script's code (comments and string
 * literals are left alone), e.g. a class renamed with its file.
 *
 * @return how many files changed
 */
int renameIdentifier(const std::filesystem::path& scriptsFolder, const std::string& from, const std::string& to);

} // namespace scriptFiles
