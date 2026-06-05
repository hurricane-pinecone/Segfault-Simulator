#pragma once

#include <engine/core/scripting/iLuaApi.h>

class SampleGame;

class GameLuaApi : public sfs::ILuaApi
{
public:
  explicit GameLuaApi(SampleGame& game) : m_game(game) {}

  void registerBindings(sfs::LuaScripting& lua) override;

private:
  SampleGame& m_game;
};
