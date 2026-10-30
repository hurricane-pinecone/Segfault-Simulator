#include "playerApp.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering/backend/renderBackends.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/webgpu/webGpuRenderBackend.h"

#ifdef SFS_STATIC_GAME_MODULE
  #include "engine/runtime/scripting/gameModule.h"
#endif

#include "engine/core/util/cpuFeatures.h"

#include <SDL_filesystem.h>
#include <SDL_messagebox.h>
#include <SDL_stdinc.h>
#include <SDL_video.h>

#include <cctype>
#include <exception>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>

  #include <cstdio>
#endif

namespace
{
// Every game's folder for its own files (the log) is under this one.
constexpr const char* kOrganization = "SegfaultSimulator";

// A game's name as a folder name: what a file system takes anywhere.
std::string folderName(const std::string& name)
{
  std::string folder;
  for (const char c : name)
    folder += std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.' ? c : '_';
  while (!folder.empty() && (folder.back() == ' ' || folder.back() == '.'))
    folder.pop_back(); // Windows drops these
  return folder.empty() ? std::string("Game") : folder;
}

// A Windows game has no console of its own; started from a terminal, its log
// goes there as well.
void attachParentConsole()
{
#if defined(_WIN32)
  if (AttachConsole(ATTACH_PARENT_PROCESS))
  {
    std::FILE* ignored = nullptr;
    freopen_s(&ignored, "CONOUT$", "w", stdout);
    freopen_s(&ignored, "CONOUT$", "w", stderr);
  }
#endif
}
} // namespace

PlayerApp::~PlayerApp()
{
  // If it stopped before destroy() (an error as it started): the scenes and
  // scriptable objects go before the module their code is in.
  sceneManager.destroyAll();
  sfs::scriptableObjects().clear();
  sfs::Logger::flush(); // a repeat count still held back
  if (m_logSink >= 0)
    sfs::Logger::removeSink(m_logSink);
  if (m_log)
    m_log->out.flush();
}

bool PlayerApp::load()
{
  attachParentConsole();
  const char* base = SDL_GetBasePath();
  m_folder = base ? std::filesystem::path(base) : std::filesystem::current_path();
  SDL_free(const_cast<char*>(base));

  // What's logged before the log file is open (reading game.json, whose name
  // says where it goes) is kept for it.
  std::vector<std::string> early;
  m_logSink = sfs::Logger::addSink([&early](sfs::Logger::Level, const std::string& message)
                                   { early.push_back(message); });
  std::optional<sfs::GameManifest> manifest = sfs::GameManifest::read(m_folder);
  if (manifest)
    m_manifest = std::move(*manifest);
  sfs::Logger::removeSink(m_logSink);
  m_logSink = -1;
  openLog(early);

  // The engine's physics is built for AVX2 (see missingCpuFeatures): without
  // it the game would crash at the first physics step, so say so now.
  if (const std::string missing = sfs::missingCpuFeatures(); !missing.empty())
  {
    fail("This game needs a processor with " + missing +
         " support, which this computer's doesn't have (or its operating system doesn't enable).");
    return false;
  }
  if (!manifest)
  {
    fail("This game is missing its game.json, or it can't be read.");
    return false;
  }
  return true;
}

void PlayerApp::openLog(const std::vector<std::string>& early)
{
  // The game's own folder for its files (%APPDATA%\SegfaultSimulator\<game> on
  // Windows, ~/.local/share/SegfaultSimulator/<game> on Linux): next to the
  // executable may not be writable (Program Files, a Steam library).
  const std::string game = folderName(m_manifest.name.empty() ? m_folder.parent_path().filename().string() : m_manifest.name);
  std::filesystem::path folder;
  if (char* pref = SDL_GetPrefPath(kOrganization, game.c_str()))
  {
    folder = std::filesystem::path(pref);
    SDL_free(pref);
  }
  else
    folder = m_folder;

  // The last run's log is kept beside this one's: the one before a crash is
  // usually the one wanted.
  m_logPath = folder / "log.txt";
  std::error_code error;
  if (std::filesystem::exists(m_logPath, error))
  {
    const std::filesystem::path previous = folder / "log.prev.txt";
    std::filesystem::remove(previous, error);
    std::filesystem::rename(m_logPath, previous, error);
  }
  m_log = std::make_unique<LogFile>();
  m_log->out.open(m_logPath, std::ios::trunc);
  if (!m_log->out)
  {
    m_log.reset();
    m_logPath.clear();
    LOG_WARN("Can't write a log file in " + folder.string());
    return;
  }
  for (const std::string& line : early)
    m_log->out << line << '\n';

  // Written out on a warning or an error (what comes before a crash), and at
  // least every second, rather than on every line.
  m_logSink = sfs::Logger::addSink(
      [log = m_log.get()](sfs::Logger::Level level, const std::string& message)
      {
        {
          const std::lock_guard lock(log->mutex);
          log->out << message << '\n';
        }
        log->flushIfDue(level >= sfs::Logger::Level::WARN);
      });
  LOG_INFO("Logging to " + m_logPath.string());
}

int PlayerApp::play()
{
  if (!load())
    return 1;
  if (!init(m_manifest.windowWidth, m_manifest.windowHeight))
  {
    fail("The game couldn't start: no graphics driver it can use was found "
         "(it needs Vulkan, Direct3D 12, Metal or OpenGL). Updating the "
         "graphics driver may help.");
    return 1;
  }
  setMaxFrameRate(m_manifest.maxFrameRate);
  // The start scene's scripts run as it starts: a throw there ends the game
  // like one in a frame (run() then only tears down).
  try
  {
    setup();
  }
  catch (const std::exception& error)
  {
    fail(std::string("The game stopped as it started, because of an error: ") + error.what());
  }
  run();
  return m_failed ? 1 : 0;
}

std::unique_ptr<sfs::IRenderBackend> PlayerApp::makeRenderBackend()
{
  // Every backend the player has, the game's choice first (another is tried
  // if it can't start here).
  sfs::registerWebGpuBackend();
  const std::vector<std::string> names = sfs::RenderBackends::names();
  const std::string& choice = m_manifest.backend.empty() ? names.front() : m_manifest.backend;
  std::unique_ptr<sfs::IRenderBackend> backend = sfs::RenderBackends::make(choice);
  if (!backend)
    LOG_ERROR("This player has no '" + choice + "' renderer");
  else
    backend->setVsync(m_manifest.vsync);
  return backend;
}

void PlayerApp::onSetup()
{
  setDebugUiVisible(false); // a shipped game shows only itself
  if (window && !m_manifest.name.empty())
    SDL_SetWindowTitle(window, m_manifest.name.c_str());

  // Scripts first: the scene's components and game objects live in the module
  // (compiled into this player on the web, which can't load libraries).
#ifdef SFS_STATIC_GAME_MODULE
  sfsRegisterModule(sfs::reflect::types());
#else
  if (!m_manifest.module.empty() &&
      !m_module.load(m_folder / m_manifest.module, m_manifest.moduleName))
  {
    fail("The game's code (" + m_manifest.module.string() + ") couldn't be loaded.");
    return;
  }
#endif

  if (!playScene(m_manifest.startScene))
  {
    fail("The start scene (" + m_manifest.startScene.string() + ") couldn't be loaded.");
    return;
  }
  LOG_INFO("Playing " + m_manifest.name);
}

void PlayerApp::onUpdate(double)
{
  // Once a second: a repeat count held back, then the file.
  const auto now = std::chrono::steady_clock::now();
  if (!m_log || now - m_logFlushed < std::chrono::seconds(1))
    return;
  m_logFlushed = now;
  sfs::Logger::flush();
  m_log->flushIfDue(true);
}

void PlayerApp::onLoadScene(const std::filesystem::path& scene)
{
  if (!playScene(scene))
    fail("The scene " + scene.generic_string() + " couldn't be loaded.");
}

void PlayerApp::onDestroy()
{
  // The scene's code lives in the module, and so may the types of the
  // scriptable objects it loaded: all gone before it's unloaded.
  sceneManager.destroyAll();
  sfs::scriptableObjects().clear();
  m_module.unload();
}

void PlayerApp::fail(const std::string& message)
{
  isRunning = false;
  LOG_ERROR(message);
  if (std::exchange(m_failed, true))
    return;
  const std::string title = m_manifest.name.empty() ? std::string("Game") : m_manifest.name;
  // Works without a window too (when one couldn't be made).
  const std::string details =
      m_logPath.empty() ? std::string() : "\n\nDetails are in " + m_logPath.string();
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), (message + details).c_str(), window);
}
