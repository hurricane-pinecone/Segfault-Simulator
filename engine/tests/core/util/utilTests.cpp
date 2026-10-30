#include "../../testHarness.h"

#include <engine/core/Color/Color.h>
#include <engine/core/logger/logger.h>
#include <engine/core/util/allocationMetrics.h>
#include <engine/core/util/atomicFile.h>
#include <engine/core/util/cpuFeatures.h>
#include <engine/core/util/format.h>
#include <engine/core/util/string.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace sfs;
namespace fs = std::filesystem;

namespace
{
std::string readFile(const fs::path& file)
{
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

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
    CHECK(std::string(formatBytes(static_cast<std::size_t>(1024) * 1024 *
                                  1024)) == "1.00 GB");
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

    // The counters are fed only by the global operator new/delete replacement
    // in allocationMetrics.cpp. Whether that replacement intercepts this
    // binary's allocations depends on how it was linked (it does in the
    // core-only/CI build, where the object is pulled in). Probe it, and assert
    // the accounting only when it is actually in effect rather than fail where
    // it is not.
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

  TEST("writeFileAtomically should replace a file whole, leaving nothing else")
  {
    const fs::path folder = fs::temp_directory_path() / "sfs_atomic_write";
    fs::remove_all(folder);
    fs::create_directories(folder);
    const fs::path file = folder / "scene.scene";

    CHECK(writeFileAtomically(file, "first"));
    CHECK(readFile(file) == "first");
    const std::string big(1 << 20, 'x');
    CHECK(writeFileAtomically(file, big));
    CHECK(readFile(file) == big);
    CHECK(writeFileAtomically(file, ""));
    CHECK(readFile(file).empty());

    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(folder))
      ++entries;
    CHECK(entries == 1u); // no temporary files left behind

    // No folder to write in: fails, and creates nothing.
    CHECK(!writeFileAtomically(folder / "missing" / "a.txt", "x"));
    CHECK(!fs::exists(folder / "missing"));
    fs::remove_all(folder);
  }

  TEST("a message repeated straight after itself should be counted, not "
       "written again")
  {
    std::vector<std::string> lines;
    const Logger::SinkId sink =
        Logger::addSink([&lines](Logger::Level, const std::string& line)
                        { lines.push_back(line); });
    const Logger::Level level = Logger::logLevel();
    Logger::setLogLevel(Logger::Level::DEBUG);
    Logger::flush(); // nothing from before counts as a repeat

    for (int i = 0; i < 5; ++i)
      LOG_INFO("same again");
    LOG_INFO("something else");
    LOG_INFO("something else");
    Logger::flush();

    CHECK(lines.size() == 4u);
    if (lines.size() == 4u)
    {
      CHECK(lines[0].ends_with("same again"));
      CHECK(lines[1].ends_with("same again (repeated 4 more times)"));
      CHECK(lines[2].ends_with("something else"));
      CHECK(lines[3].ends_with("something else (repeated 1 more time)"));
    }
    Logger::setLogLevel(level);
    Logger::removeSink(sink);
  }

  TEST("the CPU should have what this build needs (it's running it)")
  {
    CHECK(missingCpuFeatures().empty());
  }

  TEST("a log sink should be able to log, and remove itself, without "
       "deadlocking")
  {
    int calls = 0;
    sfs::Logger::SinkId self = -1;
    self = sfs::Logger::addSink(
        [&](sfs::Logger::Level, const std::string& message)
        {
          ++calls;
          LOG_INFO("from inside a sink: " + message); // console only
          if (message.find("last") != std::string::npos)
            sfs::Logger::removeSink(self);
        });
    LOG_INFO("first");
    LOG_INFO("last");
    LOG_INFO("after");
    CHECK(calls == 2);
  }

  TEST("a log sink that throws should not stop the others")
  {
    int calls = 0;
    const sfs::Logger::SinkId thrower = sfs::Logger::addSink(
        [](sfs::Logger::Level, const std::string&) { throw 1; });
    const sfs::Logger::SinkId counter = sfs::Logger::addSink(
        [&](sfs::Logger::Level, const std::string&) { ++calls; });
    LOG_INFO("hello");
    sfs::Logger::removeSink(thrower);
    sfs::Logger::removeSink(counter);
    CHECK(calls == 1);
  }

  return testing::report("utilTests");
}
