#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sfs
{

/**
 * Describes an exported game: `game.json`, next to the game's executable (the
 * engine's player). The editor writes it on export; the player reads it to know
 * what to load. Paths are relative to the game's folder.
 */
struct GameManifest
{
  static constexpr const char* kFileName = "game.json";
  static constexpr int kFormatVersion = 1;

  std::string name;               // window title
  std::string moduleName;         // the module's registration name
  std::filesystem::path module;   // the game module library
  std::filesystem::path startScene;
  int windowWidth = 1600;
  int windowHeight = 900;

  /** Read `folder/game.json`; nothing (logged) if missing or invalid. */
  static std::optional<GameManifest> read(const std::filesystem::path& folder);

  /** Write `folder/game.json`. */
  bool write(const std::filesystem::path& folder) const;
};

} // namespace sfs
