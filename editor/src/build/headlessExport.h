#pragma once

#include <filesystem>

/**
 * `SfsEditor --export <project> [folder]`: build the project's module and
 * export the game without opening the editor (for scripts and CI). Uses the
 * project's start scene (else its first scene); exports into `folder`, or the
 * project's builds/<platform>.
 *
 * @return the process exit code (0 on success).
 */
int exportHeadless(const std::filesystem::path& projectFolder,
                   const std::filesystem::path& outputFolder);

/**
 * `SfsEditor --export-web <project> [folder]`: build the project as a web page
 * (index.html and its files) into `folder`, or the project's builds/web. Needs
 * Emscripten where the editor builds (see ModuleBuilder::startWeb()).
 *
 * @return the process exit code (0 on success).
 */
int exportWebHeadless(const std::filesystem::path& projectFolder,
                      const std::filesystem::path& outputFolder);
