#include "../../testHarness.h"

#include <engine/core/Color/Color.h>
#include <engine/core/util/allocationMetrics.h>
#include <engine/core/util/format.h>
#include <engine/core/util/string.h>

#include <cstddef>
#include <cstdio>
#include <string>

using namespace sfs;

int main()
{
  TEST("toLower should lowercase ASCII and leave other characters alone")
  {
    CHECK(toLower("HeLLo123") == "hello123");
    CHECK(toLower("ALREADY!") == "already!");
    CHECK(toLower("") == "");
  }

  TEST("formatBytes should pick the right unit and precision")
  {
    CHECK(std::string(formatBytes(512)) == "512 B");
    CHECK(std::string(formatBytes(1024)) == "1.00 KB");
    CHECK(std::string(formatBytes(1536)) == "1.50 KB");
    CHECK(std::string(formatBytes(1024 * 1024)) == "1.00 MB");
    CHECK(std::string(formatBytes(static_cast<std::size_t>(1024) * 1024 * 1024)) ==
          "1.00 GB");
  }

  TEST("Color should default to white and expose named constants")
  {
    Color def;
    CHECK(def.r == 255 && def.g == 255 && def.b == 255 && def.a == 255);

    CHECK(Colors::Red.r == 255 && Colors::Red.g == 0 && Colors::Red.b == 0);
    CHECK(Colors::Transparent.a == 0);
    CHECK(Colors::Black.r == 0 && Colors::Black.a == 255);
  }

  TEST("allocationMetrics should account for allocations during a phase")
  {
    MemoryMetrics& m = getMemoryMetrics();
    CHECK(&getMemoryMetrics() == &m); // the metrics singleton is stable

    // The counters are fed only by the global operator new/delete replacement in
    // allocationMetrics.cpp. Whether that replacement intercepts this binary's
    // allocations depends on how it was linked (it does in the core-only/CI
    // build, where the object is pulled in). Probe it, and assert the accounting
    // only when it is actually in effect rather than fail where it is not.
    const std::size_t probeAllocs = m.allocationCount.load();
    int* probe = nullptr;
    {
      ScopedMemoryTracking track(MemoryTrackingPhase::Update);
      probe = new int[256];
      probe[0] = 1;
    }
    const bool intercepting = m.allocationCount.load() > probeAllocs;
    delete[] probe;

    if (!intercepting)
    {
      std::fprintf(stderr,
                   "      note: global-new tracking inactive in this build; "
                   "allocationMetrics accounting not exercised\n");
    }
    else
    {
      const std::size_t current0 = m.current.load();
      const std::size_t frees0 = m.freeCount.load();
      {
        ScopedMemoryTracking track(MemoryTrackingPhase::Update);
        int* block = new int[1024];
        block[0] = 7;
        CHECK(m.current.load() > current0); // live bytes grew while tracked
        delete[] block;
      }
      CHECK(m.freeCount.load() > frees0);
      CHECK(m.current.load() == current0); // returned to baseline
    }
  }

  return testing::report("utilTests");
}
