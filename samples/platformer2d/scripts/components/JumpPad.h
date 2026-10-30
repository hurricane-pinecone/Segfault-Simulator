#pragma once
#include <sfs/sfs.h>

// Bounces the runner high when it lands on it.
struct JumpPad
{
  float launch = 1750.0f; // upward speed it gives (world units a second)
};
