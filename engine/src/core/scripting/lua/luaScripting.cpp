#include "engine/core/scripting/luaScripting.h"

#include "engine/core/Color/Color.h"
#include "engine/core/logger/logger.h"
#include "engine/core/scripting/iLuaApi.h"
#include "engine/core/scripting/iLuaConfigurable.h"
#include "engine/core/scripting/luaSchema.h"

#include <lua.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>

namespace sfs
{

namespace
{

// `print(...)` / `sfs.log(...)`: stringify all args (tab-separated, like Lua's
// print) and route the line to the owning host's captured output + the logger.
int luaLog(lua_State* L)
{
  const int n = lua_gettop(L);
  std::string line;
  for (int i = 1; i <= n; ++i)
  {
    if (i > 1)
      line += "\t";
    const char* s = luaL_tolstring(L, i, nullptr); // pushes the string form
    line += s ? s : "";
    lua_pop(L, 1);
  }

  auto* self = *static_cast<LuaScripting**>(lua_getextraspace(L));
  if (self)
    self->logLine(line);
  return 0;
}

// Run a C-binding body, converting any C++ exception into a Lua error instead of
// letting it propagate through Lua's C frames (Lua is built as C -- an exception
// crossing a longjmp frame is undefined behaviour). The body has fully unwound
// before luaL_error longjmps, so this is safe. Game-provided lambdas / config
// hooks are the realistic throwers.
template <typename Fn>
int guarded(lua_State* L, Fn&& body)
{
  try
  {
    return body();
  }
  catch (const std::exception& e)
  {
    return luaL_error(L, "%s", e.what());
  }
  catch (...)
  {
    return luaL_error(L, "unknown C++ exception in bound function");
  }
}

// Capped Lua allocator: tracks live bytes and fails an allocation that would push
// past the budget (Lua turns the null return into an out-of-memory error, caught
// by pcall). `ud` is the host's LuaMemoryUsage. When ptr is null, osize is a Lua
// type tag, not a real size -- so old size counts only for an existing block.
void* cappedAlloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
  auto* mem = static_cast<LuaMemoryUsage*>(ud);
  const size_t oldSize = ptr ? osize : 0;

  if (nsize == 0)
  {
    std::free(ptr);
    mem->used -= oldSize;
    return nullptr;
  }

  if (mem->cap > 0 && nsize > oldSize &&
      mem->used + (nsize - oldSize) > mem->cap)
    return nullptr; // over budget -> reported as out-of-memory

  void* block = std::realloc(ptr, nsize);
  if (block) // on failure realloc leaves the old block intact; used unchanged
    mem->used = mem->used - oldSize + nsize;
  return block;
}

// Instruction-count hook: aborts a chunk that blows its budget (e.g. an infinite
// loop) instead of hanging the thread / freezing the browser tab.
void instructionGuard(lua_State* L, lua_Debug* /*ar*/)
{
  luaL_error(L,
             "execution aborted: instruction budget exceeded (possible "
             "infinite loop)");
}

// Last-resort: an unprotected Lua error would otherwise abort() silently. All our
// entry points run under lua_pcall, so this should never fire -- but log if it
// does. (Returning still aborts; the value is the diagnostic.)
int panicHandler(lua_State* L)
{
  const char* msg = lua_tostring(L, -1);
  LOG_ERROR(std::string("Lua PANIC: ") + (msg ? msg : "unknown error"));
  return 0;
}

// Trampolines: the bound std::function pointer rides as the closure's upvalue.
int callVoid(lua_State* L)
{
  auto* fn = static_cast<std::function<void()>*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  return guarded(L,
                 [&]
                 {
                   if (fn && *fn)
                     (*fn)();
                   return 0;
                 });
}

int callNumber(lua_State* L)
{
  auto* fn = static_cast<std::function<void(double)>*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  return guarded(L,
                 [&]
                 {
                   if (fn && *fn)
                     (*fn)(luaL_optnumber(L, 1, 0.0));
                   return 0;
                 });
}

int callNumber2(lua_State* L)
{
  auto* fn = static_cast<std::function<void(double, double)>*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  return guarded(L,
                 [&]
                 {
                   if (fn && *fn)
                     (*fn)(luaL_optnumber(L, 1, 0.0), luaL_optnumber(L, 2, 0.0));
                   return 0;
                 });
}

// ILuaConfigurable table closures: the upvalue is an invalidatable SLOT
// (ILuaConfigurable**) so unregisterConfig can null it -- a torn-down config then
// reads as empty / ignores writes instead of dangling.
ILuaConfigurable* configUpvalue(lua_State* L)
{
  auto* slot =
      static_cast<ILuaConfigurable**>(lua_touserdata(L, lua_upvalueindex(1)));
  return slot ? *slot : nullptr;
}

int configGet(lua_State* L)
{
  return guarded(L,
                 [&]
                 {
                   ILuaConfigurable* config = configUpvalue(L);
                   if (config)
                     luaschema::pushValues(
                         L, config->luaConfigData(), config->luaConfigSchema());
                   else
                     lua_newtable(L);
                   return 1;
                 });
}

int configSet(lua_State* L)
{
  return guarded(L,
                 [&]
                 {
                   ILuaConfigurable* config = configUpvalue(L);
                   if (!config)
                     return 0;
                   luaL_checktype(L, 1, LUA_TTABLE);
                   luaschema::readTable(L,
                                        1,
                                        config->luaConfigData(),
                                        config->luaConfigSchema());
                   config->onLuaConfigChanged();
                   return 0;
                 });
}

std::string indentOf(int depth)
{
  return std::string(static_cast<std::size_t>(depth) * 2, ' ');
}

// Render a Lua value at `idx` for the console: scalars plainly, strings quoted,
// tables JSON-style pretty-printed (indented, one entry per line). Multi-line
// keeps long results readable and -- crucially -- narrow, so the output doesn't
// blow out the side panel's width.
std::string valueToString(lua_State* L, int idx, int depth)
{
  const int type = lua_type(L, idx);

  if (type == LUA_TSTRING)
    return std::string("\"") + lua_tostring(L, idx) + "\"";

  if (type == LUA_TFUNCTION)
    return "function";

  if (type == LUA_TTABLE && depth < 4)
  {
    const int table = lua_absindex(L, idx);
    std::string body;
    lua_pushnil(L);
    while (lua_next(L, table) != 0)
    {
      // key at -2, value at -1. Prefix "key = " for string keys only (printing
      // a numeric/array key via lua_tostring would corrupt the lua_next
      // iterator).
      body += indentOf(depth + 1);
      if (lua_type(L, -2) == LUA_TSTRING)
      {
        body += lua_tostring(L, -2);
        body += " = ";
      }
      body += valueToString(L, -1, depth + 1);
      body += ",\n";
      lua_pop(L, 1); // pop value, keep key for the next lua_next
    }

    if (body.empty())
      return "{}";
    return "{\n" + body + indentOf(depth) + "}";
  }

  const char* str = luaL_tolstring(L, idx, nullptr); // pushes the string form
  std::string out = str ? str : "";
  lua_pop(L, 1);
  return out;
}

std::vector<std::string> splitPath(const std::string& path)
{
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= path.size())
  {
    const std::size_t dot = path.find('.', start);
    if (dot == std::string::npos)
    {
      parts.push_back(path.substr(start));
      break;
    }
    parts.push_back(path.substr(start, dot - start));
    start = dot + 1;
  }
  return parts;
}

void setColorField(lua_State* L, const char* name, const Color& c)
{
  // The `colors` table is on top; set colors[name] = {r, g, b, a}.
  lua_newtable(L);
  lua_pushinteger(L, c.r);
  lua_setfield(L, -2, "r");
  lua_pushinteger(L, c.g);
  lua_setfield(L, -2, "g");
  lua_pushinteger(L, c.b);
  lua_setfield(L, -2, "b");
  lua_pushinteger(L, c.a);
  lua_setfield(L, -2, "a");
  lua_setfield(L, -2, name);
}

} // namespace

LuaScripting::LuaScripting() = default;

LuaScripting::~LuaScripting()
{
  if (m_state)
    lua_close(m_state);
}

bool LuaScripting::init()
{
  // Custom allocator so the memory budget covers every allocation from creation.
  m_state = lua_newstate(&cappedAlloc, &m_memory);
  if (!m_state)
    return false;

  // Stash the host in the state's extra space so the C functions (luaLog) can
  // reach it without an upvalue.
  *static_cast<LuaScripting**>(lua_getextraspace(m_state)) = this;

  lua_atpanic(m_state, &panicHandler);

  // Open a SAFE subset of the standard library: no io / os / package, which
  // both sandboxes the live console and keeps the wasm build off filesystem /
  // dlopen syscalls that don't exist in the browser.
  const luaL_Reg safeLibs[] = {
      {LUA_GNAME, luaopen_base},
      {LUA_TABLIBNAME, luaopen_table},
      {LUA_STRLIBNAME, luaopen_string},
      {LUA_MATHLIBNAME, luaopen_math},
      {LUA_COLIBNAME, luaopen_coroutine},
      {LUA_UTF8LIBNAME, luaopen_utf8},
  };
  for (const luaL_Reg& lib : safeLibs)
  {
    luaL_requiref(m_state, lib.name, lib.func, 1);
    lua_pop(m_state, 1);
  }

  // Close sandbox holes the base lib leaves open even without io/os/package:
  //   - load: can compile arbitrary text AND raw bytecode (crafted bytecode can
  //     crash the VM); we don't expose dynamic code loading to scripts.
  //   - loadfile / dofile: read the filesystem via C stdio, bypassing the io
  //     exclusion (native only, but absent on the safe surface regardless).
  for (const char* unsafe : {"load", "loadfile", "dofile"})
  {
    lua_pushnil(m_state);
    lua_setglobal(m_state, unsafe);
  }

  // Build the `sfs` API table.
  lua_newtable(m_state); // sfs

  lua_pushcfunction(m_state, &luaLog);
  lua_setfield(m_state, -2, "log");

  // Also expose the capturing logger as the global `print` (replaces the base
  // print) so a REPL chunk's print() lines show in the console output box.
  lua_pushcfunction(m_state, &luaLog);
  lua_setglobal(m_state, "print");

  // sfs.colors: a real table (so it's usable AND introspectable for editor
  // autocomplete via keysOf / Lua's own pairs). Mirror sfs::Colors.
  lua_newtable(m_state); // colors
  setColorField(m_state, "White", Colors::White);
  setColorField(m_state, "Black", Colors::Black);
  setColorField(m_state, "Red", Colors::Red);
  setColorField(m_state, "Green", Colors::Green);
  setColorField(m_state, "Blue", Colors::Blue);
  setColorField(m_state, "Yellow", Colors::Yellow);
  setColorField(m_state, "Cyan", Colors::Cyan);
  setColorField(m_state, "Magenta", Colors::Magenta);
  setColorField(m_state, "Orange", Colors::Orange);
  setColorField(m_state, "Purple", Colors::Purple);
  setColorField(m_state, "Pink", Colors::Pink);
  setColorField(m_state, "Gray", Colors::Gray);
  setColorField(m_state, "LightGray", Colors::LightGray);
  setColorField(m_state, "DarkGray", Colors::DarkGray);
  setColorField(m_state, "Brown", Colors::Brown);
  setColorField(m_state, "Lime", Colors::Lime);
  setColorField(m_state, "Navy", Colors::Navy);
  setColorField(m_state, "Turqoise", Colors::Turqoise);
  setColorField(m_state, "Transparent", Colors::Transparent);
  lua_setfield(m_state, -2, "colors");

  lua_setglobal(m_state, "sfs");
  return true;
}

void LuaScripting::armExecutionGuard()
{
  if (m_instructionLimit > 0)
    lua_sethook(m_state, &instructionGuard, LUA_MASKCOUNT, m_instructionLimit);
  else
    lua_sethook(m_state, nullptr, 0, 0);
}

std::string LuaScripting::eval(const std::string& source)
{
  if (!m_state)
    return "Lua VM not initialised";

  armExecutionGuard();
  if (luaL_loadstring(m_state, source.c_str()) != LUA_OK ||
      lua_pcall(m_state, 0, 0, 0) != LUA_OK)
  {
    const char* err = lua_tostring(m_state, -1);
    std::string message = err ? err : "unknown Lua error";
    lua_pop(m_state, 1);
    return message;
  }

  return {};
}

std::string LuaScripting::evalRepl(const std::string& source)
{
  if (!m_state)
    return "error: Lua VM not initialised";

  m_log.clear();
  m_outputTruncated = false;
  const int base = lua_gettop(m_state);

  // Try the input as an expression first (so a bare value prints), else run it
  // as statements.
  const std::string asExpr = "return " + source;
  if (luaL_loadstring(m_state, asExpr.c_str()) != LUA_OK)
  {
    lua_pop(m_state, 1); // discard the expression-form compile error
    if (luaL_loadstring(m_state, source.c_str()) != LUA_OK)
    {
      const char* err = lua_tostring(m_state, -1);
      std::string message =
          std::string("error: ") + (err ? err : "compile error");
      lua_pop(m_state, 1);
      return message;
    }
  }

  armExecutionGuard();
  if (lua_pcall(m_state, 0, LUA_MULTRET, 0) != LUA_OK)
  {
    const char* err = lua_tostring(m_state, -1);
    std::string message =
        std::string("error: ") + (err ? err : "runtime error");
    lua_pop(m_state, 1);
    return cappedOutput(m_log + message); // show whatever printed before the error
  }

  std::string out;
  for (int i = base + 1; i <= lua_gettop(m_state); ++i)
  {
    if (!out.empty())
      out += "\t";
    out += valueToString(m_state, i, 0);
  }
  lua_settop(m_state, base); // drop the results

  // Captured print/log output first, then the expression's value (if any).
  std::string result = m_log;
  if (!out.empty())
  {
    if (!result.empty() && result.back() != '\n')
      result += "\n";
    result += out;
  }
  return cappedOutput(std::move(result));
}

std::string LuaScripting::cappedOutput(std::string text)
{
  if (m_outputLimit > 0 && text.size() > m_outputLimit)
  {
    text.resize(m_outputLimit);
    m_outputTruncated = true;
  }
  if (m_outputTruncated)
    text += "\n...[output truncated]";
  return text;
}

void LuaScripting::logLine(const std::string& line)
{
  LOG_INFO(line); // the logger always gets the full line

  // Bound the captured buffer so a tight print loop can't balloon it between
  // truncation checks; the full result is capped again in cappedOutput.
  if (m_outputLimit > 0 && m_log.size() >= m_outputLimit)
  {
    m_outputTruncated = true;
    return;
  }
  m_log += line;
  m_log += "\n";
}

void LuaScripting::bind(const std::string& name, std::function<void()> fn)
{
  if (!m_state)
    return;

  m_voidCallbacks.push_back(
      std::make_unique<std::function<void()>>(std::move(fn)));
  lua_pushlightuserdata(m_state, m_voidCallbacks.back().get());
  lua_pushcclosure(m_state, &callVoid, 1);
  lua_setglobal(m_state, name.c_str());
}

void LuaScripting::bind(const std::string& name, std::function<void(double)> fn)
{
  if (!m_state)
    return;

  m_numberCallbacks.push_back(
      std::make_unique<std::function<void(double)>>(std::move(fn)));
  lua_pushlightuserdata(m_state, m_numberCallbacks.back().get());
  lua_pushcclosure(m_state, &callNumber, 1);
  lua_setglobal(m_state, name.c_str());
}

void LuaScripting::bind(const std::string& name,
                        std::function<void(double, double)> fn)
{
  if (!m_state)
    return;

  m_number2Callbacks.push_back(
      std::make_unique<std::function<void(double, double)>>(std::move(fn)));
  lua_pushlightuserdata(m_state, m_number2Callbacks.back().get());
  lua_pushcclosure(m_state, &callNumber2, 1);
  lua_setglobal(m_state, name.c_str());
}

void LuaScripting::registerApi(ILuaApi& api) { api.registerBindings(*this); }

void LuaScripting::registerConfig(ILuaConfigurable& config)
{
  if (!m_state)
    return;

  // The closures share one invalidatable slot (ILuaConfigurable**), owned here.
  m_configSlots.push_back(std::make_unique<ILuaConfigurable*>(&config));
  ILuaConfigurable** slot = m_configSlots.back().get();

  lua_newtable(m_state);

  lua_pushlightuserdata(m_state, slot);
  lua_pushcclosure(m_state, &configGet, 1);
  lua_setfield(m_state, -2, "get");

  lua_pushlightuserdata(m_state, slot);
  lua_pushcclosure(m_state, &configSet, 1);
  lua_setfield(m_state, -2, "set");

  // Schema doc (key -> hint), generated from the config so autocomplete and the
  // editable surface never drift apart.
  luaschema::pushSchema(m_state, config.luaConfigSchema());
  lua_setfield(m_state, -2, "options");

  lua_setglobal(m_state, config.luaConfigName().c_str());
}

void LuaScripting::unregisterConfig(ILuaConfigurable& config)
{
  if (!m_state)
    return;

  // Null every slot pointing at this config -- any lingering get/set closures
  // (even ones Lua still references) become no-ops.
  for (std::unique_ptr<ILuaConfigurable*>& slot : m_configSlots)
    if (*slot == &config)
      *slot = nullptr;

  lua_pushnil(m_state);
  lua_setglobal(m_state, config.luaConfigName().c_str());
}

std::vector<std::string> LuaScripting::keysOf(const std::string& path) const
{
  std::vector<std::string> keys;
  if (!m_state || path.empty())
    return keys;

  const std::vector<std::string> parts = splitPath(path);

  // Resolve the dotted path onto the stack, keeping only the current value.
  lua_getglobal(m_state, parts[0].c_str());
  for (std::size_t i = 1; i < parts.size(); ++i)
  {
    if (!lua_istable(m_state, -1))
    {
      lua_pop(m_state, 1);
      return keys;
    }
    lua_getfield(m_state, -1, parts[i].c_str());
    lua_remove(m_state, -2); // drop the parent table
  }

  if (lua_istable(m_state, -1))
  {
    lua_pushnil(m_state);
    while (lua_next(m_state, -2) != 0)
    {
      // key at -2, value at -1. Only collect string keys (and don't run
      // lua_tostring on non-strings -- that would corrupt the iterator key).
      if (lua_type(m_state, -2) == LUA_TSTRING)
        keys.emplace_back(lua_tostring(m_state, -2));
      lua_pop(m_state, 1); // pop value, keep key for the next lua_next
    }
  }

  lua_pop(m_state, 1); // pop the resolved value
  std::sort(keys.begin(), keys.end());
  return keys;
}

} // namespace sfs
