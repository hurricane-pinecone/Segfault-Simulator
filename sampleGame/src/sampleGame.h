
#pragma once

#include <SDL_events.h>
#include <engine/runtime/game/game.h>
#include <engine/runtime/rendering/iQuadRenderer.h>
#include <engine/runtime/rendering/util/isometric/geometry.h>
#include <engine/runtime/sceneManager/scene.h>
#include <engine/core/scripting/luaScripting.h>
#include <memory>
#include <string>

class SampleGame : public sfs::Game
{
public:
  SampleGame() = default;
  ~SampleGame() = default;

  // The active scene, for Lua bindings that reach into it at call time.
  sfs::Scene* currentScene() { return sceneManager.current(); }

protected:
  void onSetup() override;
  void onProcessInput(const sfs::Input& input) override;
  void onUpdate(double deltaTime) override;
  void onDestroy() override;

  std::unique_ptr<sfs::IQuadRenderer>
  createQuadRenderer(int windowWidth, int windowHeight) override;

private:
  // Live Lua VM, owned at the app level so it persists across scenes. Bindings
  // reach the active scene's systems lazily (resolved at call time).
  void setupLua();

  sfs::IsometricProjectionConfig m_isoConfig;
  sfs::IsometricProjection m_isoProjection;

  std::unique_ptr<sfs::LuaScripting> m_lua;
};
