#pragma once

namespace sfs
{

class LuaScripting;

// Contract a game implements to expose a Lua modding API. registerBindings() is
// the single place a client binds its globals and tables on the VM -- compose
// it from the building blocks: LuaScripting::bind() (simple globals),
// registerConfig() (an ILuaConfigurable object), registerParticleLua() (the
// engine's particle table), or the raw API for anything bespoke.
//
// The host installs it with LuaScripting::registerApi(api). A game can split
// its surface into several ILuaApi modules (combat, world, ...) and install
// each.
class ILuaApi
{
public:
  virtual ~ILuaApi() = default;

  // Bind this module's globals/tables onto the VM. Called once at install.
  virtual void registerBindings(LuaScripting& lua) = 0;
};

} // namespace sfs
