#pragma once

#include "engine/core/scripting/luaSchema.h"
#include <string>

namespace sfs
{

// Contract a game class implements to expose a live-editable config to Lua.
// LuaScripting::registerConfig() auto-builds a global table named
// luaConfigName(), entirely driven by luaConfigSchema():
//
//   <name>.get()      -> a table of the object's current values
//   <name>.set{ ... } -> apply edits to the live object, then
//   onLuaConfigChanged() <name>.options    -> the schema doc (key -> hint) for
//   autocomplete
//
// So a client slaps this on a class, returns its field schema, registers it,
// and gets a self-describing live editor for free -- no raw Lua C API.
//
// It adds a vtable, so it's for ordinary classes, NOT for POD descs that must
// stay aggregates (e.g. ParticleEffectDesc, whose loader contract depends on
// aggregate-ness) -- those keep a free-function schema instead.
class ILuaConfigurable
{
public:
  virtual ~ILuaConfigurable() = default;

  // Global table name this config is exposed under (e.g. "sun").
  virtual std::string luaConfigName() const = 0;

  // The editable field surface. The fields' member offsets are taken relative
  // to luaConfigData().
  virtual LuaSchema luaConfigSchema() const = 0;

  // The live object the schema's offsets apply to -- normally `this`, returned
  // from the most-derived type so the offsets land correctly under multiple
  // inheritance.
  virtual void* luaConfigData() = 0;

  // Called after a set{} writes the raw fields, so the object can clamp /
  // validate / react (the writes bypass any setters). Default: nothing.
  virtual void onLuaConfigChanged() {}
};

} // namespace sfs
