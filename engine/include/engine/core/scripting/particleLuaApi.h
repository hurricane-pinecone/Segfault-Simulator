#pragma once

#include <functional>
#include <string>

namespace sfs
{

class LuaScripting;
class ParticleEngine;

// Register the engine's base particle modding API on the VM as a global table:
//
//   <name>.spawn(effect, x, y)     -- one-shot burst at a world tile (ground
//                                     elevation from the engine's terrain
//                                     source)
//   <name>.configure(effect, opts) -- live-tweak a registered effect (schema-
//                                     driven; opts mirrors particle.options)
//   <name>.describe(effect)        -- the effect's current configurable values
//   <name>.effects()               -- names of every registered effect
//   <name>.options                 -- the configure schema (key -> hint)
//
// This is the reusable core every game shares; a game wraps/curates it for its
// own modding API. The live ParticleEngine is resolved lazily through `resolve`
// (so the table keeps working across scene changes / module rebuilds); a null
// return makes the calls no-ops. `resolve` is owned by the VM and outlives it.
void registerParticleLua(LuaScripting& lua,
                         const std::string& tableName,
                         std::function<ParticleEngine*()> resolve);

} // namespace sfs
