#pragma once
#include <sfs/sfs.h>

// A pickup the runner grabs by touching it: score, and a step up the combo.
struct Orb
{
  int value = 10;
  float spin = 0.0f; // the spin animation's clock
};
