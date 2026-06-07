#pragma once

#include "sampleGame.h"
#include <engine/core/scripting/luaScripting.h>

// Debug-overlay Lua bindings (inline). registerDebugBindings() exposes live
// control of the ImGui debug UI (stats + sliders) so it can be shown or hidden
// from the console.
namespace gamebindings
{

// showDebug(1) / showDebug(0) -- force the debug overlay on or off.
// toggleDebug()               -- flip its current visibility.
inline void registerDebugBindings(sfs::LuaScripting& lua, SampleGame& game)
{
  lua.bind(
      "showDebug", [&game](double on) { game.setDebugUiVisible(on != 0.0); });

  lua.bind("toggleDebug", [&game] { game.toggleDebugUi(); });
}

} // namespace gamebindings
