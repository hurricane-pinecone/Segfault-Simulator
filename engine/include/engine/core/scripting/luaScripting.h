#pragma once

#include <cstddef>
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

class ILuaApi;
class ILuaConfigurable;

// Live byte accounting for the VM's capped allocator (see setMemoryLimit). `cap`
// of 0 means unlimited.
struct LuaMemoryUsage
{
  std::size_t used = 0;
  std::size_t cap = 0;
};

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
   * REPL-style eval for an interactive console: if the whole input is a single
   * expression it is evaluated and its value(s) are returned stringified
   * (tables are expanded one level, e.g. "{a, b}" / "{k=v}"); otherwise it runs
   * as statements and returns "". Errors come back prefixed with "error: ".
   * Never throws.
   */
  std::string evalRepl(const std::string& source);

  // Append a line to the current eval's captured output (what `print` / sfs.log
  // write). evalRepl returns this prepended to the expression's value, so a
  // multi-statement chunk that prints shows every line. Also logged.
  void logLine(const std::string& line);

  /**
   * Bind a global Lua callable to a C++ function. Two arities cover the common
   * live-edit cases: a no-arg trigger and a single-number setter. The game
   * passes a std::function, so it stays Lua-free.
   */
  void bind(const std::string& name, std::function<void()> fn);
  void bind(const std::string& name, std::function<void(double)> fn);
  void bind(const std::string& name, std::function<void(double, double)> fn);

  /**
   * Install a game's modding API: just calls api.registerBindings(*this). The
   * verb keeps the host as the driver and reads symmetrically with registerConfig.
   */
  void registerApi(ILuaApi& api);

  /**
   * Expose an ILuaConfigurable as a live-editable global table (get / set / options),
   * driven entirely by its schema. See ILuaConfigurable for the generated surface.
   *
   * The table reaches the config through an invalidatable slot, so it is safe to
   * destroy the config before the VM AS LONG AS unregisterConfig() is called
   * first -- after that, the table's get/set become no-ops even if Lua still
   * holds a reference to it.
   */
  void registerConfig(ILuaConfigurable& config);

  /**
   * Invalidate a registered config: nils its global table and makes any lingering
   * get/set no-ops. Call before the config is destroyed (e.g. from its owner's
   * destructor) so a torn-down object can't be reached from Lua.
   */
  void unregisterConfig(ILuaConfigurable& config);

  /**
   * Per-eval instruction budget (a Lua count hook). A chunk exceeding it is
   * aborted with an error instead of hanging the thread -- essential for an
   * interactive console (an infinite loop would otherwise freeze the app, and
   * the browser tab on web). <= 0 disables the guard. Re-armed before each
   * eval/evalRepl, so it is a per-call budget, not a lifetime total.
   */
  void setInstructionLimit(int maxInstructions) { m_instructionLimit = maxInstructions; }

  /**
   * Cap total bytes the VM may allocate (a custom Lua allocator). An allocation
   * past the cap fails as a Lua out-of-memory error (caught, not a crash), so a
   * script can't exhaust host memory. 0 = unlimited. Safe to set before or after
   * init(); the cap is read live. `memoryUsed()` reports current bytes.
   */
  void setMemoryLimit(std::size_t bytes) { m_memory.cap = bytes; }
  std::size_t memoryUsed() const { return m_memory.used; }

  /**
   * Cap the size (bytes) of the string evalRepl returns -- captured print/log
   * output plus the value. A script printing megabytes is truncated with a
   * marker instead of bloating the response handed to the editor. 0 = unlimited.
   */
  void setOutputLimit(std::size_t bytes) { m_outputLimit = bytes; }

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
  // (Re)install the instruction-count hook before a chunk runs.
  void armExecutionGuard();

  // Truncate evalRepl output to m_outputLimit, appending a marker if anything
  // (here or in logLine) was dropped.
  std::string cappedOutput(std::string text);

  lua_State* m_state = nullptr;

  // Byte budget for the capped allocator (passed to it as user data). Declared
  // before m_state so it is still alive when ~LuaScripting closes the state
  // (lua_close frees through this allocator). cap default ~64 MiB.
  LuaMemoryUsage m_memory{0, 64 * 1024 * 1024};

  // Per-eval instruction budget for the execution guard (0 = off).
  int m_instructionLimit = 20'000'000;

  // Max bytes evalRepl returns (0 = unlimited).
  std::size_t m_outputLimit = 256 * 1024;

  // Output captured during the current evalRepl (from print / sfs.log).
  std::string m_log;

  // Set when print/log output or the result was dropped for the output cap.
  bool m_outputTruncated = false;

  // Heap "slots" pointing at registered configs. The Lua closures hold a slot,
  // not the config directly, so unregisterConfig() can null a slot and instantly
  // neuter a table whose backing object is about to be destroyed.
  std::vector<std::unique_ptr<ILuaConfigurable*>> m_configSlots;

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

// The host last passed to setActiveLua(), or nullptr. Lets app code (e.g. a
// scene registering its ILuaConfigurable systems) reach the live VM without threading
// a back-pointer through the scene graph.
LuaScripting* activeLua();

} // namespace sfs
