#include "engine/utils/allocationMetrics.h"
#include <tracy/Tracy.hpp>

#include <cstdlib>
#include <new>

namespace
{
thread_local sfs::MemoryTrackingPhase currentPhase =
    sfs::MemoryTrackingPhase::None;

thread_local bool insideTracker = false;
} // namespace

namespace sfs
{

MemoryMetrics& getMemoryMetrics()
{
  static MemoryMetrics metrics;
  return metrics;
}

MemoryTrackingPhase getMemoryTrackingPhase() { return currentPhase; }

static void setMemoryTrackingPhase(MemoryTrackingPhase phase)
{
  currentPhase = phase;
}

struct alignas(std::max_align_t) AllocationHeader
{
  std::size_t size;
  bool tracked;
};

static void recordAllocation(std::size_t size)
{
  auto& metrics = getMemoryMetrics();

  metrics.allocated += size;
  metrics.allocationCount++;

  const auto current = metrics.current.fetch_add(size) + size;

  auto peak = metrics.peak.load();
  while (current > peak && !metrics.peak.compare_exchange_weak(peak, current))
  {
  }
}

static void recordFree(std::size_t size)
{
  auto& metrics = getMemoryMetrics();

  metrics.freed += size;
  metrics.freeCount++;
  metrics.current -= size;
}

ScopedMemoryTracking::ScopedMemoryTracking(MemoryTrackingPhase phase)
    : previousPhase(getMemoryTrackingPhase())
{
  setMemoryTrackingPhase(phase);
}

ScopedMemoryTracking::~ScopedMemoryTracking()
{
  setMemoryTrackingPhase(previousPhase);
}

} // namespace sfs

void* operator new(std::size_t size)
{
  const auto totalSize = size + sizeof(sfs::AllocationHeader);

  auto* raw = static_cast<unsigned char*>(std::malloc(totalSize));

  if (!raw)
    throw std::bad_alloc();

  auto* header = reinterpret_cast<sfs::AllocationHeader*>(raw);

  const auto phase = sfs::getMemoryTrackingPhase();

  header->size = size;
  header->tracked = phase != sfs::MemoryTrackingPhase::None && !insideTracker;

  void* userMemory = raw + sizeof(sfs::AllocationHeader);

  TracyAlloc(userMemory, size);

  if (header->tracked)
  {
    insideTracker = true;
    sfs::recordAllocation(size);
    insideTracker = false;
  }

  return userMemory;
}

void operator delete(void* memory) noexcept
{
  if (!memory)
    return;

  auto* raw =
      static_cast<unsigned char*>(memory) - sizeof(sfs::AllocationHeader);

  auto* header = reinterpret_cast<sfs::AllocationHeader*>(raw);

  TracyFree(memory);

  if (header->tracked && !insideTracker)
  {
    insideTracker = true;
    sfs::recordFree(header->size);
    insideTracker = false;
  }

  std::free(raw);
}

void* operator new[](std::size_t size) { return operator new(size); }

void operator delete[](void* memory) noexcept { operator delete(memory); }

void operator delete(void* memory, std::size_t) noexcept
{
  operator delete(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
  operator delete[](memory);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  try
  {
    return operator new(size);
  }
  catch (...)
  {
    return nullptr;
  }
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
  operator delete(memory);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  return operator new(size, std::nothrow);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
  operator delete[](memory);
}
