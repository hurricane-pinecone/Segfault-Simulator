#include "playerApp.h"

// An exported game: plays the game described by game.json next to the
// executable (see sfs::GameManifest).
int main(int, char*[])
{
#ifdef __EMSCRIPTEN__
  // In a browser, run() hands the loop to the page and main's stack unwinds
  // (destructors and all, with WebAssembly exceptions on): the player has to
  // outlive it.
  static PlayerApp player;
#else
  PlayerApp player;
#endif
  if (!player.load())
    return 1;
  if (!player.init(player.manifest().windowWidth, player.manifest().windowHeight))
    return 1;
  player.setup();
  player.run();
  return 0;
}
