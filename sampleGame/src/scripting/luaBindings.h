#pragma once

namespace sfs
{
class LuaScripting;
}

class SampleGame;

// Register the sample game's Lua modding API on the VM: the `splat` /
// `spawnGore` globals and the `particles` table (spawn / configure / effects /
// options). The bindings reach the active scene through `game` at call time, so
// they work across scene changes. The app only wires this up; the API itself
// lives here.
void registerGameLua(sfs::LuaScripting& lua, SampleGame& game);
