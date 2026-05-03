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

  game.init(800, 600);
  game.setup();
  game.run();
  game.destroy();

  return 0;
}
