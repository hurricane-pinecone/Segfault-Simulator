#pragma once

#include "sfs/codegen/rules.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// A tree's lint settings: the .sfs-lint file nearest above a file.
namespace sfs::lsp
{

/** What a `.sfs-lint` file turns on and off. */
struct LintConfig
{
  /** Whether the house rules are on (`include: house`). */
  bool house = false;
  /** The rules it turns off (`disable: rule, ...`). */
  codegen::RuleSet disabled;
};

/**
 * Reads a `.sfs-lint` file's text: `key: value` lines, `#` starting a comment,
 * blank lines skipped. `include: house` turns the house rules on, and
 * `disable: rule, ...` turns warning rules off; both may be given more than
 * once. What it doesn't know is skipped with a warning.
 *
 * @param text the file's text
 * @param file the file's path, for the warnings
 * @param warnings gains one line per problem (`file:line: ...`)
 * @return the settings it gives
 */
LintConfig parseLintConfig(std::string_view text,
                           const std::filesystem::path& file,
                           std::vector<std::string>& warnings);

/**
 * The nearest `.sfs-lint` file above `file`: in its folder, then each folder
 * above it.
 *
 * @param file the file being checked (an absolute path; a relative one is
 *   looked up only through the folders it names)
 * @return the config file's path, or empty when there is none
 */
std::filesystem::path findLintConfig(const std::filesystem::path& file);

/**
 * The lint settings for `file`: those of its nearest `.sfs-lint`, or none (the
 * house rules off) without one. A config file is read again when it changes,
 * and its warnings go to stderr each time it's read.
 *
 * @param file the file being checked
 * @return its settings
 */
LintConfig lintConfigFor(const std::filesystem::path& file);

} // namespace sfs::lsp
