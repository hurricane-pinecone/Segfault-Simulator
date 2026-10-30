#pragma once

#include "engine/core/logger/logger.h"
#include "engine/runtime/game/game.h"
#include "engine/runtime/game/gameManifest.h"
#include "engine/runtime/scripting/moduleHost.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Plays an exported game: loads its module and start scene (named by game.json)
// into the engine's standard scene, starts its scripts and runs it -- what
// the editor's Play does, without the editor.
class PlayerApp : public sfs::Game
{
public:
  ~PlayerApp() override;

  /** Read game.json from the executable's folder; false (logged) if missing. */
  bool load();

  /**
   * Load, start and run the game, as main() does: the exit code. What stops
   * it from starting is told to the player (see fail()).
   */
  int play();

  const sfs::GameManifest& manifest() const { return m_manifest; }

  /**
   * Tell the player why the game can't run (there's no console to read): a
   * message box naming the log, and the game ends.
   */
  void fail(const std::string& message);

protected:
  std::unique_ptr<sfs::IRenderBackend> makeRenderBackend() override;
  void onSetup() override;
  void onDestroy() override;
  // Writes the log file out once a second, so a crash or a kill loses at most
  // the last second of it.
  void onUpdate(double deltaTime) override;
  void onLoadScene(const std::filesystem::path& scene) override;
  void onFatalError(const std::string& message) override { fail(message); }
  std::filesystem::path gameFolder() const override { return m_folder; }

private:
  // Shown once: a failure while stopping after another says nothing new.
  bool m_failed = false;

  /**
   * Start the log file, log.txt in the game's folder for its own files (see
   * SDL_GetPrefPath), the previous run's kept as log.prev.txt; `early` is
   * what was logged before it opened.
   */
  void openLog(const std::vector<std::string>& early);

  std::chrono::steady_clock::time_point m_logFlushed;
  struct LogFile
  {
    std::mutex mutex; // threads log too
    std::ofstream out;
    std::chrono::steady_clock::time_point flushed = std::chrono::steady_clock::now();

    // Write what's held back if it's been a second (or `now`).
    void flushIfDue(bool now = false)
    {
      const std::lock_guard lock(mutex);
      const auto time = std::chrono::steady_clock::now();
      if (now || time - flushed > std::chrono::seconds(1))
      {
        out.flush();
        flushed = time;
      }
    }
  };

  std::filesystem::path m_folder;
  std::unique_ptr<LogFile> m_log;
  std::filesystem::path m_logPath; // empty if there's no log file
  sfs::Logger::SinkId m_logSink = -1;
  sfs::GameManifest m_manifest;
  sfs::ModuleHost m_module;
};
