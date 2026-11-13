#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

/** What upgradeNodeIds() found, checked and did. */
struct NodeIdUpgradeReport
{
  /** Every check passed and, when writing, every changed file was written. */
  bool ok = false;
  /** How many prefab and scene files were read. */
  std::size_t files = 0;
  /** One file with old model node ids. */
  struct ChangedFile
  {
    /** Its path, relative to the project. */
    std::string path;
    /** How many of its paths name model nodes by old ids. */
    std::size_t edits = 0;
  };
  /** The files with old model node ids, in path order (scenes after prefabs). */
  std::vector<ChangedFile> changed;
  /** How many "$overrides" keys name model nodes by old ids. */
  std::size_t overrideKeys = 0;
  /** How many "$removed" paths name model nodes by old ids. */
  std::size_t removedPaths = 0;
  /** How many "$added" entities' "$parent"s name model nodes by old ids. */
  std::size_t addedParents = 0;
  /** How many of those old ids are past their model's nodes (kept as "old:<id>", naming nothing). */
  std::size_t oldIdsKept = 0;
  /** Each instance whose prefab can't be read ("<file>: <prefab>"): its paths can't be upgraded, so the check fails. */
  std::vector<std::string> unreadable;
};

/**
 * Upgrade a project's prefabs and scenes from model nodes' old ids (their place in the model file's node order) to
 * their stable ids (see stableNodeId()), by editing only those strings in each file (and its "formatVersion"), so
 * every other byte stays as it was. Files with nothing to change aren't touched.
 *
 * Before anything is written, every check must pass: every instance's prefab can be read; each edited file parses to
 * exactly the in-memory upgrade of the file as it was; every edited path names a node with the same name path before
 * (by its old id) and after (by its stable id); every entry of every prefab and scene expands to the same entities
 * from the old files and from the upgraded ones; and upgrading the upgraded files would change nothing. Then every
 * upgraded file is staged beside its file before the first is replaced, and a failed replace puts the replaced ones
 * back. On any failure no file is changed (logged).
 *
 * @param project the project's folder (its assets/, prefabs/ and scenes/ are read)
 * @param write   false: only check and report (`--check`)
 * @return what was found, and whether it all passed
 */
NodeIdUpgradeReport upgradeNodeIds(const std::filesystem::path& project, bool write);

/**
 * `SfsEditor --upgrade-node-ids <project> [--check]`: upgradeNodeIds() without opening the editor, logging what it
 * found per file and in total.
 *
 * @return the process exit code (0 when every check passed)
 */
int upgradeNodeIdsHeadless(const std::filesystem::path& project, bool checkOnly);

/**
 * `SfsEditor --list-node-ids <model>`: print a model file's nodes, in the file's order, each with its old id (its
 * place in that order), its stable id and its name path.
 *
 * @return the process exit code (0 if the model could be read)
 */
int listNodeIdsHeadless(const std::filesystem::path& model);
