#pragma once

#include "engine/core/scripting/luaSchema.h"

namespace sfs
{

// The Lua-editable surface of a ParticleEffectDesc, declared once: the
// reflection reader drives configure (table -> desc), the `options`
// autocomplete doc, and a values dump from this single list. Lua-free -- it's
// just field descriptors -- so it compiles into both the native and web builds.
// The `color` field is documented here but applied by the caller (it maps onto
// a Gradient).
const LuaSchema& particleEffectSchema();

} // namespace sfs
