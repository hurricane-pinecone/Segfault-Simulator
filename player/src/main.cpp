#include "playerApp.h"

// An exported game: plays the game described by game.json next to the
// executable (see sfs::GameManifest).
int main(int, char*[])
{
  PlayerApp player;
  if (!player.load())
    return 1;
  if (!player.init(player.manifest().windowWidth, player.manifest().windowHeight))
    return 1;
  player.setup();
  player.run();
  return 0;
}
