#include "config.h"
#include "sampleGame.h"
#include <cstddef>
#include <cstdlib>
#include <engine/core/logger/logger.h>
#include <engine/runtime/game/game.h>

int main(int argc, char* argv[])
{
  SampleGame game;

  sfs::Logger::setLogLevel(sfs::Logger::Level::DEBUG);
  // sfs::Logger::setVerbosity(sfs::Logger::Verbosity::FULL);

  if (!game.init(WINDOW_WIDTH, WINDOW_HEIGHT))
    return 1;
  game.setup();
  game.run();

  return 0;
}
