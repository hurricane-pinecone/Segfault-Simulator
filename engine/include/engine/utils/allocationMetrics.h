#pragma once

#include <atomic>
#include <cstddef>

namespace sfs
{

enum class MemoryTrackingPhase
{
  None,
  Setup,
  Update,
  Render
};

struct MemoryMetrics
{
  std::atomic<std::size_t> allocated{0};
  std::atomic<std::size_t> freed{0};
  std::atomic<std::size_t> current{0};
  std::atomic<std::size_t> peak{0};
  std::atomic<std::size_t> allocationCount{0};
  std::atomic<std::size_t> freeCount{0};
};

MemoryMetrics& getMemoryMetrics();

MemoryTrackingPhase getMemoryTrackingPhase();

class ScopedMemoryTracking
{
public:
  explicit ScopedMemoryTracking(MemoryTrackingPhase phase);
  ~ScopedMemoryTracking();

  ScopedMemoryTracking(const ScopedMemoryTracking&) = delete;
  ScopedMemoryTracking& operator=(const ScopedMemoryTracking&) = delete;

private:
  MemoryTrackingPhase previousPhase;
};

} // namespace sfs
