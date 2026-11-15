#pragma once

#include <sfs/sfs.h>

// Declared in SparkSystem.cpp, not here: the module's build scans its
// sources for event types too.
struct SparkEvent;

// Sends a spark each update and counts them with a handler.
class SparkSystem : public sfs::System
{
protected:
  void create() override;
  void update(double deltaTime) override;

private:
  sfs::EventConsumer<SparkEvent> m_sparks;
  int m_count = 0;
};
