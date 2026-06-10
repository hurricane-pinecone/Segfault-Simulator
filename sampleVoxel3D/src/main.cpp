#include "voxel3dGame.h"

int main(int /*argc*/, char* /*argv*/[])
{
  Voxel3DGame game;
  if (!game.init(1280, 720))
    return 1;
  game.setup();
  game.run();
  return 0;
}
