#pragma once

#include "engine/core/logger/logger.h"
#include "engine/runtime/game/game.h"
#include "engine/runtime/game/gameManifest.h"
#include "engine/runtime/scripting/moduleHost.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

// Plays an exported game: loads its module and start scene (named by game.json)
// into the engine's standard 3D scene, starts its scripts and runs it -- what
// the editor's Play does, without the editor.
class PlayerApp : public sfs::Game
{
public:
  ~PlayerApp() override;

  /** Read game.json from the executable's folder; false (logged) if missing. */
  bool load();

  const sfs::GameManifest& manifest() const { return m_manifest; }

protected:
  std::unique_ptr<sfs::IRenderBackend> makeRenderBackend() override;
  void onSetup() override;
  void onDestroy() override;

private:
  /** Tell the player why the game can't run (there's no console to read). */
  void fail(const std::string& message);

  std::filesystem::path m_folder;
  std::unique_ptr<std::ofstream> m_log; // log.txt next to the executable
  sfs::Logger::SinkId m_logSink = -1;
  sfs::GameManifest m_manifest;
  sfs::ModuleHost m_module;
};
