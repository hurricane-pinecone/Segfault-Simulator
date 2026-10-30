#pragma once
#include <sfs/sfs.h>

// A lamp that comes on as the light goes: brighter the darker it is.
struct Lantern
{
  [[sfs::range(0, 10)]] float nightIntensity = 3.2f;
  [[sfs::range(0, 10)]] float dayIntensity = 0.6f;
};
