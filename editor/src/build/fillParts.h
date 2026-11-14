#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/** What fillProjectParts() found and did. */
struct FillPartsReport
{
  /**
   * Every prefab asked for could be read, and each changed one edited (and,
   * when writing, written).
   */
  bool ok = false;
  /** How many prefabs were read. */
  std::size_t prefabs = 0;
  /** One prefab whose part fields changed. */
  struct ChangedPrefab
  {
    /** Its path, relative to the project. */
    std::string path;
    /** How many of its fields changed. */
    std::size_t fields = 0;
  };
  /** The prefabs whose part fields changed, in path order. */
  std::vector<ChangedPrefab> changed;
};

/**
 * Fill the [[sfs::part]] fields of a project's prefabs from the names of the
 * entities below them, as the Inspector's Fill Parts does (parts::fillAll()),
 * with the component types registered now (a game's with its module loaded).
 * Each prefab is opened as the editor's prefab tab opens it. A nested
 * instance of another .prefab is left to that prefab's own file.
 *
 * A prefab whose fields change is edited in place: only the changed values'
 * text is replaced (or inserted, in key order, where the file lacks them), in
 * the editor's style, so its diff is just the part values. Before anything is
 * written, the edited file must open to exactly what was filled; one that
 * doesn't, or that uses old component names, is left as it was (logged).
 * What's filled, and what can't be, is logged.
 *
 * @param project the project's folder
 * @param which   "all", or one prefab's path relative to the project
 *                ("prefabs/Tractor.prefab")
 * @param write   false: a dry run that writes nothing
 * @return what was found and done
 */
FillPartsReport fillProjectParts(const std::filesystem::path& project,
                                 std::string_view which,
                                 bool write);
