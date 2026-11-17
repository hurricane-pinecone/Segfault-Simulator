#pragma once

#include "engine/core/logger/logger.h"
#include "engine/runtime/game/game.h"
#include "engine/runtime/game/gameManifest.h"
#include "engine/runtime/rendering/backend/iRenderBackend.h"
#include "engine/runtime/scripting/moduleHost.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Plays an exported game: loads its module and start scene (named by game.json)
// into the engine's standard scene, starts its scripts and runs it -- what
// the editor's Play does, without the editor.
class PlayerApp : public sfs::Game
{
public:
  ~PlayerApp() override;

  /**
   * Tell the player why the game can't run (there's no console to read): a
   * message box naming the log, and the game ends.
   */
  void fail(const std::string& message);

  /** Read game.json from the executable's folder; false (logged) if missing. */
  bool load();

  const sfs::GameManifest& manifest() const { return m_manifest; }

  /**
   * Load, start and run the game, as main() does: the exit code. What stops
   * it from starting is told to the player (see fail()).
   */
  int play();

protected:
  std::filesystem::path gameFolder() const override { return m_folder; }
  std::unique_ptr<sfs::IRenderBackend> makeRenderBackend() override;
  void onDestroy() override;
  void onFatalError(const std::string& message) override { fail(message); }
  void onLoadScene(const std::filesystem::path& scene) override;
  void onSetup() override;
  // Writes the log file out once a second, so a crash or a kill loses at most
  // the last second of it.
  void onUpdate(double deltaTime) override;

private:
  // The log file, written on a thread of its own: a line logged is only
  // queued, so no game frame waits on the disk (or on another thread's
  // write). Written out at least every second and soon after a warning; an
  // error waits till it's on disk (what comes before a crash).
  struct LogFile
  {
    // Flushed soon (doesn't wait).
    void flushSoon();
    // Queued; `soon`: flushed as soon as it's written.
    void push(std::string line, bool soon);
    void start();
    // Everything written, the thread ended.
    void stop();
    // Everything queued so far written and flushed (waits).
    void sync();

    std::ofstream out;
    std::mutex mutex;
    std::condition_variable wake, written;
    std::vector<std::string> queued;
    uint64_t pushed = 0, done = 0; // lines queued, lines written and flushed
    bool flushWanted = false, stopping = false;
    std::thread writer;
  };

  /**
   * Start the log file, log.txt in the game's folder for its own files (see
   * SDL_GetPrefPath), the previous run's kept as log.prev.txt; `early` is
   * what was logged before it opened.
   */
  void openLog(const std::vector<std::string>& early);

  // Shown once: a failure while stopping after another says nothing new.
  bool m_failed = false;
  std::chrono::steady_clock::time_point m_logFlushed;
  std::filesystem::path m_folder;
  std::unique_ptr<LogFile> m_log;
  std::filesystem::path m_logPath; // empty if there's no log file
  sfs::Logger::SinkId m_logSink = -1;
  sfs::GameManifest m_manifest;
  sfs::ModuleHost m_module;
};
