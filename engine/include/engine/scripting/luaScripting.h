#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-declared so this header pulls in no Lua headers: it's safe to include
// anywhere (including the web build, which links no Lua). The implementation
// lives under src/scripting/lua/ and is excluded from the web build.
struct lua_State;

namespace sfs
{

/**
 * Embedded Lua VM for live scripting -- hot-editing the running game without a
 * rebuild. The state persists across eval() calls (the heap-allocated VM lives
 * for the host's lifetime), so re-running a chunk mutates the live game rather
 * than restarting it. On the web build this is the path a JS editor would drive
 * through an exported eval entry point; natively it backs an in-app console.
 *
 * The public API is deliberately Lua-free: the game binds behaviour by handing
 * over std::functions, so game code needs no Lua headers.
 */
class LuaScripting
{
public:
  LuaScripting();
  ~LuaScripting();

  LuaScripting(const LuaScripting&) = delete;
  LuaScripting& operator=(const LuaScripting&) = delete;

  /**
   * Open the VM, the safe standard libraries, and the built-in `sfs` table
   * (sfs.colors from sfs::Colors, sfs.log). Returns false if the VM could not
   * be created.
   */
  bool init();

  /**
   * Run a chunk against the persistent state. Returns an empty string on
   * success, otherwise the Lua compile/runtime error message (never throws --
   * errors are caught via lua_pcall so a bad edit reports instead of aborting).
   */
  std::string eval(const std::string& source);

  /**
   * Bind a global Lua callable to a C++ function. Two arities cover the common
   * live-edit cases: a no-arg trigger and a single-number setter. The game
   * passes a std::function, so it stays Lua-free.
   */
  void bind(const std::string& name, std::function<void()> fn);
  void bind(const std::string& name, std::function<void(double)> fn);
  void bind(const std::string& name, std::function<void(double, double)> fn);

  /**
   * Sorted field names of the table at a dotted path (e.g. "sfs.colors"), or
   * empty if the path isn't a table. This is the primitive an editor calls to
   * build autocomplete for the bound API.
   */
  std::vector<std::string> keysOf(const std::string& path) const;

  /**
   * Raw VM handle, for callers that need to push richer bindings than the
   * std::function arities above (e.g. functions reading Lua tables). Null until
   * init(). The caller must include the Lua headers itself.
   */
  lua_State* state() { return m_state; }

private:
  lua_State* m_state = nullptr;

  // The bound std::functions, kept alive for the C trampolines that reference
  // them by pointer (stored as Lua closure upvalues).
  std::vector<std::unique_ptr<std::function<void()>>> m_voidCallbacks;
  std::vector<std::unique_ptr<std::function<void(double)>>> m_numberCallbacks;
  std::vector<std::unique_ptr<std::function<void(double, double)>>>
      m_number2Callbacks;
};

// Register the host that the web entry points (sfsEvalLua / sfsLuaKeys, called
// from the JS editor) and any other free-function caller routes to. The app
// sets this to its LuaScripting instance; pass nullptr on teardown.
void setActiveLua(LuaScripting* lua);

} // namespace sfs
