#include "config.h"
#include "platformerGame.h"

#include <engine/core/logger/logger.h>

int main(int /*argc*/, char* /*argv*/[])
{
  PlatformerGame game;

  sfs::Logger::setLogLevel(sfs::Logger::Level::DEBUG);

  if (!game.init(WINDOW_WIDTH, WINDOW_HEIGHT))
    return 1;

  game.setup();
  game.run();

  return 0;
}
