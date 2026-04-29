
#include "game/game.h"
#include "logger/logger.h"
#include <cstddef>
#include <cstdlib>

int main(int argc, char* argv[])
{
  Game game;

  Logger::setLogLevel(Logger::Level::DEBUG);

  game.init();
  game.run();
  game.destroy();

  return 0;
}
