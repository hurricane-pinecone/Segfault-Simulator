#include "engine/scripting/luaScripting.h"

#include "engine/Color/Color.h"
#include "engine/logger/logger.h"
#include "engine/scripting/iLuaApi.h"
#include "engine/scripting/iLuaConfig.h"
#include "engine/scripting/luaSchema.h"

#include <lua.hpp>

#include <algorithm>

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

// Trampolines: the bound std::function pointer rides as the closure's upvalue.
int callVoid(lua_State* L)
{
  auto* fn = static_cast<std::function<void()>*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  if (fn && *fn)
    (*fn)();
  return 0;
}

int callNumber(lua_State* L)
{
  auto* fn = static_cast<std::function<void(double)>*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  if (fn && *fn)
    (*fn)(luaL_optnumber(L, 1, 0.0));
  return 0;
}

int callNumber2(lua_State* L)
{
  auto* fn = static_cast<std::function<void(double, double)>*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  if (fn && *fn)
    (*fn)(luaL_optnumber(L, 1, 0.0), luaL_optnumber(L, 2, 0.0));
  return 0;
}

// ILuaConfig table closures: the config pointer rides as the upvalue, so the
// same three C functions back every registered config.
ILuaConfig* configUpvalue(lua_State* L)
{
  return static_cast<ILuaConfig*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int configGet(lua_State* L)
{
  ILuaConfig* config = configUpvalue(L);
  if (config)
    luaschema::pushValues(L, config->luaConfigData(), config->luaConfigSchema());
  else
    lua_newtable(L);
  return 1;
}

int configSet(lua_State* L)
{
  ILuaConfig* config = configUpvalue(L);
  if (!config)
    return 0;
  luaL_checktype(L, 1, LUA_TTABLE);
  luaschema::readTable(L, 1, config->luaConfigData(), config->luaConfigSchema());
  config->onLuaConfigChanged();
  return 0;
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
  m_state = luaL_newstate();
  if (!m_state)
    return false;

  // Stash the host in the state's extra space so the C functions (luaLog) can
  // reach it without an upvalue.
  *static_cast<LuaScripting**>(lua_getextraspace(m_state)) = this;

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

std::string LuaScripting::eval(const std::string& source)
{
  if (!m_state)
    return "Lua VM not initialised";

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

  if (lua_pcall(m_state, 0, LUA_MULTRET, 0) != LUA_OK)
  {
    const char* err = lua_tostring(m_state, -1);
    std::string message =
        std::string("error: ") + (err ? err : "runtime error");
    lua_pop(m_state, 1);
    return m_log + message; // show whatever printed before the error
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
  return result;
}

void LuaScripting::logLine(const std::string& line)
{
  m_log += line;
  m_log += "\n";
  LOG_INFO(line);
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

void LuaScripting::registerConfig(ILuaConfig& config)
{
  if (!m_state)
    return;

  lua_newtable(m_state);

  lua_pushlightuserdata(m_state, &config);
  lua_pushcclosure(m_state, &configGet, 1);
  lua_setfield(m_state, -2, "get");

  lua_pushlightuserdata(m_state, &config);
  lua_pushcclosure(m_state, &configSet, 1);
  lua_setfield(m_state, -2, "set");

  // Schema doc (key -> hint), generated from the config so autocomplete and the
  // editable surface never drift apart.
  luaschema::pushSchema(m_state, config.luaConfigSchema());
  lua_setfield(m_state, -2, "options");

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
