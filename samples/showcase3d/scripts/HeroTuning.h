#pragma once
#include <sfs/sfs.h>

// How the hero moves: shared data in an asset (assets/data/heroTuning.asset),
// edited in the Inspector. Metres and seconds.
struct HeroTuning : sfs::ScriptableObject
{
  [[sfs::range(1, 20)]] float runSpeed = 7.0f;
  [[sfs::range(1, 200)]] float groundAcceleration = 55.0f;
  [[sfs::range(1, 200)]] float airAcceleration = 22.0f;
  [[sfs::range(1, 20)]] float jumpSpeed = 7.2f;
  [[sfs::range(0, 3)]] int airJumps = 1;
  [[sfs::range(1, 40)]] float dashSpeed = 17.0f;
  [[sfs::range(0.05, 1)]] float dashTime = 0.18f;
  [[sfs::range(0, 0.5)]] float coyoteTime = 0.12f;
  [[sfs::range(0, 0.5)]] float jumpBuffer = 0.12f;
  [[sfs::range(0.5, 8)]] float turnSpeed = 3.2f;
};
