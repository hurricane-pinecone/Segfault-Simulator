#pragma once

#include <filesystem>
#include <string_view>

class ModuleBuilder;
class Project;

/**
 * Build a project's game module and wait for it, as the headless commands do
 * (its output logged).
 *
 * @param project the project
 * @param builder builds it; its modulePath() is the result
 * @return true if it built
 */
bool buildModuleHeadless(const Project& project, ModuleBuilder& builder);

/**
 * `SfsEditor --export [--development] <project> [folder]`: build the
 * project's module and export the game without opening the editor (for
 * scripts and CI). Uses the project's start scene (else its first scene);
 * exports into `folder`, or the project's builds/<platform>. `development`:
 * a development build (its development keys work; see
 * GameExporter::Options::development).
 *
 * @return the process exit code (0 on success).
 */
int exportHeadless(const std::filesystem::path& projectFolder,
                   const std::filesystem::path& outputFolder,
                   bool development = false);

/**
 * `SfsEditor --export-web <project> [folder]`: build the project as a web page
 * (index.html and its files) into `folder`, or the project's builds/web. Needs
 * Emscripten where the editor builds (see ModuleBuilder::startWeb()).
 *
 * @return the process exit code (0 on success).
 */
int exportWebHeadless(const std::filesystem::path& projectFolder,
                      const std::filesystem::path& outputFolder);

/**
 * `SfsEditor --fill-parts <project> <prefab|all> [--dry-run]`: build and load
 * the project's game module (its components' part fields are its own), then
 * fill its prefabs' part fields in place (fillProjectParts()), logging what
 * changed per prefab and in total. Building the module writes into the
 * project's .sfs/build (and may add its build files, as opening it in the
 * editor does), even in a dry run.
 *
 * @param projectFolder the project's folder
 * @param which         "all", or one prefab's path relative to the project
 * @param dryRun        write no prefab: only report what would be filled
 * @return the process exit code (0 if every prefab could be read, edited and,
 *         unless dry, written)
 */
int fillPartsHeadless(const std::filesystem::path& projectFolder,
                      std::string_view which,
                      bool dryRun);
