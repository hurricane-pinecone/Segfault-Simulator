#pragma once

#include <filesystem>
#include <string>
#include <vector>

class Project;

// Assembles a playable game from a project: the engine's player executable
// (renamed after the game) and its libraries, the built game module, the
// project's scenes and assets, and game.json saying what to load.
//
//   <output>/
//     <Game>.exe            SfsPlayer, renamed
//     *.dll / *.so          engine and runtime libraries
//     <Module>.dll          the game module
//     scenes/  assets/      the project's content
//     game.json             sfs::GameManifest
//
// The module must already be built (the editor builds it first).
class GameExporter
{
public:
  struct Options
  {
    std::filesystem::path output;
    std::filesystem::path startScene; // relative to the project
    std::filesystem::path module;     // the built module library
    int windowWidth = 1600;
    int windowHeight = 900;
    // A development build: its development keys work (F8 switches visual
    // styles; see sfs::Game::setDevKeys). Off for a game to ship.
    bool development = false;
  };

  /** The player executable and libraries ship from the editor's own folder. */
  explicit GameExporter(std::filesystem::path editorFolder);

  /**
   * Export `project` into `options.output`. An existing output folder is
   * replaced only if it is empty or a previous export (it has a game.json),
   * so a mistyped path can't wipe anything else.
   *
   * @return the game's executable, or empty (logged) on failure.
   */
  std::filesystem::path run(const Project& project, const Options& options) const;

  /** The game's executable name for a project name (filesystem-safe). */
  static std::string executableName(const std::string& projectName);

private:
  // Shared libraries the player needs: those next to it, plus any listed in
  // SfsPlayer.libs (engine libraries built elsewhere, as on Linux).
  std::vector<std::filesystem::path> runtimeLibraries() const;

  std::filesystem::path m_editorFolder;
};
