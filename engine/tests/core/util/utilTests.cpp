#include "../../testHarness.h"

#include <engine/core/Color/Color.h>
#include <engine/core/util/allocationMetrics.h>
#include <engine/core/util/format.h>
#include <engine/core/util/string.h>

#include <cstddef>
#include <string>

using namespace sfs;

int main()
{
  // --- toLower ------------------------------------------------------------
  {
    CHECK(toLower("HeLLo123") == "hello123");
    CHECK(toLower("ALREADY!") == "already!");
    CHECK(toLower("") == "");
  }

  // --- formatBytes: unit thresholds and precision -------------------------
  {
    CHECK(std::string(formatBytes(512)) == "512 B");
    CHECK(std::string(formatBytes(1024)) == "1.00 KB");
    CHECK(std::string(formatBytes(1536)) == "1.50 KB");
    CHECK(std::string(formatBytes(1024 * 1024)) == "1.00 MB");
    CHECK(std::string(formatBytes(static_cast<std::size_t>(1024) * 1024 * 1024)) ==
          "1.00 GB");
  }

  // --- Color: defaults and named constants --------------------------------
  {
    Color def;
    CHECK(def.r == 255 && def.g == 255 && def.b == 255 && def.a == 255);

    CHECK(Colors::Red.r == 255 && Colors::Red.g == 0 && Colors::Red.b == 0);
    CHECK(Colors::Transparent.a == 0);
    CHECK(Colors::Black.r == 0 && Colors::Black.a == 255);
  }

  // --- allocationMetrics: counts allocations made inside a tracking phase ---
  {
    MemoryMetrics& m = getMemoryMetrics();

    ScopedMemoryTracking track(MemoryTrackingPhase::Update);

    const std::size_t allocs0 = m.allocationCount.load();
    const std::size_t current0 = m.current.load();

    int* block = new int[1024];
    block[0] = 7; // keep the allocation live and used

    CHECK(m.allocationCount.load() > allocs0);
    CHECK(m.current.load() > current0);

    const std::size_t frees0 = m.freeCount.load();
    delete[] block;
    CHECK(m.freeCount.load() > frees0);
    CHECK(m.current.load() == current0); // returned to baseline
  }

  return testing::report("utilTests");
}
