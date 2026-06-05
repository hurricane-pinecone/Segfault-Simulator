#include "engine/scripting/luaScripting.h"

#include <string>

#ifdef ENGINE_WEB
  #include <emscripten.h>
  #define SFS_LUA_EXPORT EMSCRIPTEN_KEEPALIVE
#else
  #define SFS_LUA_EXPORT
#endif

// Bridge between a JS editor and the app's Lua VM. The app registers its host
// via setActiveLua(); the exported C entry points below are what the web
// shell's editor calls through Module.ccall. They live here (not in the app) so
// the export surface is stable and the app stays free of Emscripten specifics.

namespace
{
sfs::LuaScripting* g_lua = nullptr;
// Held across the synchronous ccall so the returned pointer stays valid while
// JS copies it.
std::string g_result;
} // namespace

namespace sfs
{
void setActiveLua(LuaScripting* lua) { g_lua = lua; }
LuaScripting* activeLua() { return g_lua; }
} // namespace sfs

extern "C"
{

  // Run a chunk against the live VM (REPL-style: a bare expression returns its
  // stringified value; statements return ""). Errors come back prefixed
  // "error: ".
  SFS_LUA_EXPORT const char* sfsEvalLua(const char* source)
  {
    if (!g_lua)
    {
      g_result = "error: Lua VM not ready";
      return g_result.c_str();
    }
    g_result = g_lua->evalRepl(source ? source : "");
    return g_result.c_str();
  }

  // Newline-separated field names of a dotted table path (e.g. "sfs.colors"),
  // for the editor to build autocomplete from the live bindings.
  SFS_LUA_EXPORT const char* sfsLuaKeys(const char* path)
  {
    g_result.clear();
    if (!g_lua || !path)
      return g_result.c_str();

    for (const std::string& key : g_lua->keysOf(path))
    {
      if (!g_result.empty())
        g_result.push_back('\n');
      g_result += key;
    }
    return g_result.c_str();
  }
}
