#include "config.h"
#include "voxel3dGame.h"

#include <engine/core/logger/logger.h>

int main(int /*argc*/, char* /*argv*/[])
{
  Voxel3DGame game;

  sfs::Logger::setLogLevel(sfs::Logger::Level::DEBUG);

  if (!game.init(WINDOW_WIDTH, WINDOW_HEIGHT))
    return 1;

  game.setup();
  game.run();

  return 0;
}
