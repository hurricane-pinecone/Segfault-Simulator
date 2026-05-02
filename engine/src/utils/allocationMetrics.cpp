#include <engine/utils/allocationMetrics.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace
{
std::atomic_bool& trackingEnabled()
{
  static std::atomic_bool enabled{false};
  return enabled;
}
} // namespace

namespace sfs
{

void setMemoryTrackingEnabled(bool enabled)
{
  trackingEnabled().store(enabled, std::memory_order_relaxed);
}

bool isMemoryTrackingEnabled()
{
  return trackingEnabled().load(std::memory_order_relaxed);
}

MemoryMetrics& getMemoryMetrics()
{
  static sfs::MemoryMetrics metrics;
  return metrics;
}

struct alignas(std::max_align_t) AllocationHeader
{
  std::size_t size;
  bool tracked;
};

static void recordAllocation(std::size_t size)
{
  auto& metrics = sfs::getMemoryMetrics();

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
  auto& metrics = sfs::getMemoryMetrics();

  metrics.freed += size;
  metrics.freeCount++;
  metrics.current -= size;
}

} // namespace sfs

void* operator new(std::size_t size)
{
  const auto totalSize = size + sizeof(sfs::AllocationHeader);

  auto* raw = static_cast<unsigned char*>(std::malloc(totalSize));

  if (!raw)
  {
    throw std::bad_alloc();
  }

  auto* header = reinterpret_cast<sfs::AllocationHeader*>(raw);
  header->size = size;
  header->tracked = sfs::isMemoryTrackingEnabled();

  if (header->tracked)
    sfs::recordAllocation(size);

  return raw + sizeof(sfs::AllocationHeader);
}

void operator delete(void* memory) noexcept
{
  if (!memory)
  {
    return;
  }

  auto* raw =
      static_cast<unsigned char*>(memory) - sizeof(sfs::AllocationHeader);
  auto* header = reinterpret_cast<sfs::AllocationHeader*>(raw);

  if (header->tracked)
    sfs::recordFree(header->size);

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
