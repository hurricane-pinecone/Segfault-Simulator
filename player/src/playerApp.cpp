#include "playerApp.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering/backend/renderBackends.h"
#include "engine/webgpu/webGpuRenderBackend.h"

#include <SDL_filesystem.h>
#include <SDL_messagebox.h>
#include <SDL_video.h>

PlayerApp::~PlayerApp()
{
  if (m_logSink >= 0)
    sfs::Logger::removeSink(m_logSink);
}

bool PlayerApp::load()
{
  const char* base = SDL_GetBasePath();
  m_folder = base ? std::filesystem::path(base) : std::filesystem::current_path();

  // The log goes to log.txt too: a shipped game has no console.
  m_log = std::make_unique<std::ofstream>(m_folder / "log.txt", std::ios::trunc);
  m_logSink = sfs::Logger::addSink([log = m_log.get()](sfs::Logger::Level, const std::string& message)
                       { *log << message << '\n' << std::flush; });

  std::optional<sfs::GameManifest> manifest = sfs::GameManifest::read(m_folder);
  if (!manifest)
  {
    fail("This game is missing its game.json, or it can't be read.");
    return false;
  }
  m_manifest = std::move(*manifest);
  return true;
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
  return backend;
}

void PlayerApp::onSetup()
{
  setDebugUiVisible(false); // a shipped game shows only itself
  if (window && !m_manifest.name.empty())
    SDL_SetWindowTitle(window, m_manifest.name.c_str());

  // Scripts first: the scene's components and game objects live in the module.
  if (!m_manifest.module.empty() &&
      !m_module.load(m_folder / m_manifest.module, m_manifest.moduleName))
  {
    fail("The game's code (" + m_manifest.module.string() + ") couldn't be loaded.");
    return;
  }

  if (!playScene(m_manifest.startScene))
  {
    fail("The start scene (" + m_manifest.startScene.string() + ") couldn't be loaded.");
    return;
  }
  LOG_INFO("Playing " + m_manifest.name);
}

void PlayerApp::onLoadScene(const std::filesystem::path& scene)
{
  if (!playScene(scene))
    fail("The scene " + scene.generic_string() + " couldn't be loaded.");
}

void PlayerApp::onDestroy()
{
  // The scene's code lives in the module: tear it down before unloading.
  sceneManager.destroyAll();
  m_module.unload();
}

void PlayerApp::fail(const std::string& message)
{
  LOG_ERROR(message);
  const std::string title = m_manifest.name.empty() ? std::string("Game") : m_manifest.name;
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(),
                           (message + "\n\nDetails are in log.txt.").c_str(), window);
  isRunning = false;
}
