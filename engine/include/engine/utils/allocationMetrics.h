
#pragma once

#include <atomic>
#include <cstddef>

namespace sfs
{

void setMemoryTrackingEnabled(bool enabled);
bool isMemoryTrackingEnabled();

struct MemoryMetrics
{
  std::atomic<std::size_t> allocated = 0;
  std::atomic<std::size_t> freed = 0;
  std::atomic<std::size_t> current = 0;
  std::atomic<std::size_t> peak = 0;
  std::atomic<std::size_t> allocationCount = 0;
  std::atomic<std::size_t> freeCount = 0;
};

MemoryMetrics& getMemoryMetrics();
} // namespace sfs
