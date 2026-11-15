#include "SparkSystem.h"

struct [[sfs::event_news]] SparkEvent
{
  int size = 0;
};

void SparkSystem::create()
{
  m_sparks = consumer<SparkEvent>().onEvent([this](const SparkEvent& spark)
                                            { m_count += spark.size; });
}

void SparkSystem::update(double) { send(SparkEvent{1}); }
