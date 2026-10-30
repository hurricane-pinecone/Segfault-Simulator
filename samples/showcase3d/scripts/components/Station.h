#pragma once
#include <sfs/sfs.h>

#include <string>

// A place on the island worth a look: its sign says what it shows when the
// hero is near.
struct Station
{
  std::string title;
  std::string hint;
  [[sfs::range(1, 30)]] float radius = 7.0f;
};
