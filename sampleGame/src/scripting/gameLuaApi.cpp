#include "scripting/gameLuaApi.h"

#include "scripting/bindings/debugBindings.h"
#include "scripting/bindings/particleBindings.h"
#include "scripting/bindings/playerBindings.h"
#include <engine/core/scripting/luaScripting.h>

void GameLuaApi::registerBindings(sfs::LuaScripting& lua)
{
  gamebindings::registerParticleBindings(lua, m_game);
  gamebindings::registerPlayerBindings(lua, m_game);
  gamebindings::registerDebugBindings(lua, m_game);
}
