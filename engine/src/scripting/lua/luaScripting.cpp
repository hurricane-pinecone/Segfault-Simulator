#include "engine/scripting/luaScripting.h"

#include "engine/Color/Color.h"
#include "engine/logger/logger.h"

#include <lua.hpp>

#include <algorithm>

namespace sfs
{

namespace
{

// `sfs.log(value)`: stringify any Lua value and route it to the engine logger.
int luaLog(lua_State* L)
{
  const char* msg = luaL_tolstring(L, 1, nullptr); // pushes the string form
  LOG_INFO(std::string(msg ? msg : ""));
  lua_pop(L, 1);
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
