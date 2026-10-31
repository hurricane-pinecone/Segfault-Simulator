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

  std::string name;             // window title
  std::string moduleName;       // the module's registration name
  std::filesystem::path module; // the game module library
  std::filesystem::path startScene;
  int windowWidth = 1600;
  int windowHeight = 900;
  // The render backend it prefers ("webgpu", or one the player adds; see
  // RenderBackends);
  // empty: the player's first. Another is tried if it can't start.
  std::string backend;
  // Wait for the display before showing a frame ("vsync"): no tearing, and
  // no faster than the screen shows. On unless the game turns it off.
  bool vsync = true;
  // At most this many frames a second ("maxFrameRate"); 0: no cap (beyond
  // vsync's).
  double maxFrameRate = 0.0;
  // A development build ("development"): the development keys work in it
  // (Game::setDevKeys; e.g. F8 switches visual styles). Off for a game to
  // ship.
  bool development = false;

  /** Read `folder/game.json`; nothing (logged) if missing or invalid. */
  static std::optional<GameManifest> read(const std::filesystem::path& folder);

  /** Write `folder/game.json`. */
  bool write(const std::filesystem::path& folder) const;
};

} // namespace sfs
