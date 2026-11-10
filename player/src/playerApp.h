#pragma once

#include "engine/core/logger/logger.h"
#include "engine/runtime/game/game.h"
#include "engine/runtime/game/gameManifest.h"
#include "engine/runtime/scripting/moduleHost.h"

#include <chrono>
#include <condition_variable>
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
  // The log file, written on a thread of its own: a line logged is only
  // queued, so no game frame waits on the disk (or on another thread's
  // write). Written out at least every second and soon after a warning; an
  // error waits till it's on disk (what comes before a crash).
  struct LogFile
  {
    std::ofstream out;
    std::mutex mutex;
    std::condition_variable wake, written;
    std::vector<std::string> queued;
    uint64_t pushed = 0, done = 0; // lines queued, lines written and flushed
    bool flushWanted = false, stopping = false;
    std::thread writer;

    void start();
    // Queued; `soon`: flushed as soon as it's written.
    void push(std::string line, bool soon);
    // Everything queued so far written and flushed (waits).
    void sync();
    // Flushed soon (doesn't wait).
    void flushSoon();
    // Everything written, the thread ended.
    void stop();
  };

  std::filesystem::path m_folder;
  std::unique_ptr<LogFile> m_log;
  std::filesystem::path m_logPath; // empty if there's no log file
  sfs::Logger::SinkId m_logSink = -1;
  sfs::GameManifest m_manifest;
  sfs::ModuleHost m_module;
};
