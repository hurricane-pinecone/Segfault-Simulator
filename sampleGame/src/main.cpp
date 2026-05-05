#include "sampleGame.h"
#include <cstddef>
#include <cstdlib>
#include <engine/game/game.h>
#include <engine/logger/logger.h>

int main(int argc, char* argv[])
{
  SampleGame game;

  sfs::Logger::setLogLevel(sfs::Logger::Level::DEBUG);
  // sfs::Logger::setVerbosity(sfs::Logger::Verbosity::FULL);

  if (!game.init(800, 600))
    return 1;
  game.setup();
  game.run();

  return 0;
}
