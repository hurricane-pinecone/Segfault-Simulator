#include "playerApp.h"

#include "engine/core/logger/logger.h"

#include <exception>
#include <string>

// An exported game: plays the game described by game.json next to the
// executable (see sfs::GameManifest).
int main(int, char*[])
{
#ifdef __EMSCRIPTEN__
  // In a browser, run() hands the loop to the page and main's stack unwinds
  // (destructors and all, with WebAssembly exceptions on): the player has to
  // outlive it. Nothing is caught here: the page's loop unwinds main by
  // throwing, and each frame catches its own errors (see sfs::Game).
  static PlayerApp player;
  return player.play();
#else
  PlayerApp player;
  // Each frame catches its own errors; this is for starting and stopping.
  try
  {
    return player.play();
  }
  catch (const std::exception& error)
  {
    player.fail(std::string("The game stopped because of an error: ") +
                error.what());
  }
  catch (...)
  {
    player.fail("The game stopped because of an unknown error.");
  }
  return 1;
#endif
}
