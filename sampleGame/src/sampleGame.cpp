
#include "sampleGame.h"
#include "config.h"
#include "scenes/gameScene.h"
#include "scripting/gameLuaApi.h"
#include <engine/core/components/worldCollider.h>
#include <engine/core/logger/logger.h>
#include <engine/core/scripting/luaScripting.h>
#include <engine/runtime/input/keyboardInput.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/webgpu/webGpuRenderBackend.h>
#include <memory>

void SampleGame::onSetup()
{
  // WorldCollider offset/size are authored in pixels; one world tile is
  // TILE_PIXELS px (render-scale independent).
  sfs::WorldCollider::pixelsPerUnit = TILE_PIXELS;

  // Stand up the VM before the scene: createScene() runs the scene's onInit()
  // synchronously, and that's where scene systems register their
  // ILuaConfigurable (e.g. the sun) -- so the active VM must already exist.
  setupLua();

  // TODO: Create actual title screen and refactor
  // sfs::Scene* titleScene = sceneManager.createScene("Title Scene");
  auto gameScene = sceneManager.createScene<GameScene>("Game");

  isRunning = true;
}

void SampleGame::setupLua()
{
  // Stand up the modding VM; the game's actual Lua API lives in luaBindings.
  m_lua = std::make_unique<sfs::LuaScripting>();
  if (!m_lua->init())
  {
    LOG_ERROR("Lua VM failed to initialise; scripting disabled");
    m_lua.reset(); // don't route the editor at a dead VM
    return;
  }
  sfs::setActiveLua(m_lua.get()); // route the web editor's eval entry here
  m_lua->setConsoleEnabled(true); // expose the in-app console (backtick key)

  // Install the game's modding API. GameLuaApi is transient -- registerBindings
  // binds everything onto the VM (the closures it leaves behind are VM-owned).
  GameLuaApi api(*this);
  m_lua->registerApi(api);
}

void SampleGame::onProcessInput(const sfs::Input& input)
{
  if (input.keyboard().keyPressed(sfs::Key::Escape))
  {
#ifdef ENGINE_WEB
    return;
#endif
    isRunning = false;
  }

  if (sceneManager.current()->name() == "Title Scene" &&
      input.mouse().mousePressed(sfs::MouseButton::Left))
    sceneManager.load("Game");
}

void SampleGame::onDestroy()
{
  sfs::setActiveLua(nullptr); // m_lua is destroyed after this
}

std::unique_ptr<sfs::IRenderBackend> SampleGame::makeRenderBackend()
{
  return std::make_unique<sfs::WebGpuRenderBackend>();
}
