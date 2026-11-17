#pragma once

// Lua's C API with C linkage, as lua.hpp gives it. Its headers are exported, so
// code that uses lua_State and the rest includes this one header.
// IWYU pragma: begin_exports
extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
// IWYU pragma: end_exports
