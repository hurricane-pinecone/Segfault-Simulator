#include "engine/scripting/luaSchema.h"

#include <lua.hpp>

namespace sfs::luaschema
{

namespace
{

char* memberAt(void* obj, std::size_t offset)
{
  return static_cast<char*>(obj) + offset;
}

const char* memberAt(const void* obj, std::size_t offset)
{
  return static_cast<const char*>(obj) + offset;
}

// Fetch a numeric field of the table at `table`; returns false (leaving `out`
// untouched) if the key is absent or not a number.
bool numberField(lua_State* L, int table, const char* key, double& out)
{
  lua_getfield(L, table, key);
  const bool ok = lua_isnumber(L, -1) != 0;
  if (ok)
    out = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return ok;
}

} // namespace

void readTable(lua_State* L, int tableIdx, void* obj, const LuaSchema& schema)
{
  tableIdx = lua_absindex(L, tableIdx);

  for (const LuaField& f : schema)
  {
    double v = 0.0;
    switch (f.kind)
    {
    case FieldKind::Int:
      if (numberField(L, tableIdx, f.name.c_str(), v))
        *reinterpret_cast<int*>(memberAt(obj, f.offset)) = static_cast<int>(v);
      break;

    case FieldKind::Float:
      if (numberField(L, tableIdx, f.name.c_str(), v))
        *reinterpret_cast<float*>(memberAt(obj, f.offset)) =
            static_cast<float>(v);
      break;

    case FieldKind::Bool:
      lua_getfield(L, tableIdx, f.name.c_str());
      if (lua_isboolean(L, -1))
        *reinterpret_cast<bool*>(memberAt(obj, f.offset)) =
            lua_toboolean(L, -1) != 0;
      lua_pop(L, 1);
      break;

    case FieldKind::Vec2:
    {
      double x = 0.0;
      double y = 0.0;
      lua_getfield(L, tableIdx, f.name.c_str());
      if (lua_istable(L, -1))
      {
        const int t = lua_gettop(L);
        auto* vec = reinterpret_cast<glm::vec2*>(memberAt(obj, f.offset));
        if (numberField(L, t, "x", x))
          vec->x = static_cast<float>(x);
        if (numberField(L, t, "y", y))
          vec->y = static_cast<float>(y);
      }
      lua_pop(L, 1);
      break;
    }

    case FieldKind::Range:
    {
      double lo = 0.0;
      double hi = 0.0;
      // FloatRange is {float min; float max;} -- write the two floats directly,
      // so the reader needn't know the type.
      const bool hasLo = numberField(L, tableIdx, (f.name + "Min").c_str(), lo);
      const bool hasHi = numberField(L, tableIdx, (f.name + "Max").c_str(), hi);
      if (hasLo && hasHi)
      {
        auto* range = reinterpret_cast<float*>(memberAt(obj, f.offset));
        range[0] = static_cast<float>(lo);
        range[1] = static_cast<float>(hi);
      }
      break;
    }

    case FieldKind::Color:
      break; // caller-applied (maps onto a domain-specific target)
    }
  }
}

void pushSchema(lua_State* L, const LuaSchema& schema)
{
  lua_newtable(L);
  for (const LuaField& f : schema)
  {
    if (f.kind == FieldKind::Range)
    {
      lua_pushstring(L, f.hint.c_str());
      lua_setfield(L, -2, (f.name + "Min").c_str());
      lua_pushstring(L, f.hint.c_str());
      lua_setfield(L, -2, (f.name + "Max").c_str());
    }
    else
    {
      lua_pushstring(L, f.hint.c_str());
      lua_setfield(L, -2, f.name.c_str());
    }
  }
}

void pushValues(lua_State* L, const void* obj, const LuaSchema& schema)
{
  lua_newtable(L);
  for (const LuaField& f : schema)
  {
    const char* at = memberAt(obj, f.offset);
    switch (f.kind)
    {
    case FieldKind::Int:
      lua_pushinteger(L, *reinterpret_cast<const int*>(at));
      lua_setfield(L, -2, f.name.c_str());
      break;

    case FieldKind::Float:
      lua_pushnumber(L, *reinterpret_cast<const float*>(at));
      lua_setfield(L, -2, f.name.c_str());
      break;

    case FieldKind::Bool:
      lua_pushboolean(L, *reinterpret_cast<const bool*>(at));
      lua_setfield(L, -2, f.name.c_str());
      break;

    case FieldKind::Vec2:
    {
      const auto* vec = reinterpret_cast<const glm::vec2*>(at);
      lua_newtable(L);
      lua_pushnumber(L, vec->x);
      lua_setfield(L, -2, "x");
      lua_pushnumber(L, vec->y);
      lua_setfield(L, -2, "y");
      lua_setfield(L, -2, f.name.c_str());
      break;
    }

    case FieldKind::Range:
    {
      const auto* range = reinterpret_cast<const float*>(at);
      lua_pushnumber(L, range[0]);
      lua_setfield(L, -2, (f.name + "Min").c_str());
      lua_pushnumber(L, range[1]);
      lua_setfield(L, -2, (f.name + "Max").c_str());
      break;
    }

    case FieldKind::Color:
      break; // not dumped (target type is domain-specific)
    }
  }
}

glm::vec3 readColor(lua_State* L, int idx)
{
  idx = lua_absindex(L, idx);
  const auto channel = [&](const char* key, int arrayIndex) -> float
  {
    lua_getfield(L, idx, key);
    if (!lua_isnumber(L, -1))
    {
      lua_pop(L, 1);
      lua_rawgeti(L, idx, arrayIndex);
    }
    const float v = static_cast<float>(luaL_optnumber(L, -1, 0.0));
    lua_pop(L, 1);
    return v / 255.0f;
  };
  return glm::vec3{channel("r", 1), channel("g", 2), channel("b", 3)};
}

} // namespace sfs::luaschema
