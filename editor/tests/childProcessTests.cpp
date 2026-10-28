#include "../../engine/tests/testHarness.h"

#include "build/buildEnvironment.h"
#include "build/childProcess.h"

#include <chrono>
#include <csignal>
#include <fstream>
#include <string>
#include <thread>

namespace
{
using Clock = std::chrono::steady_clock;

bool alive(int pid) { return ::kill(pid, 0) == 0; }
} // namespace

int main()
{
  TEST("terminate() should stop the process and what it started, without waiting for them")
  {
    // A shell that starts a long sleep, says which, and waits on it.
    std::string output;
    ChildProcess process;
    CHECK(process.start({"sh", "-c", "sleep 30 & echo $!; wait"},
                        [&](std::string_view chunk) { output += chunk; }));
    const auto started = Clock::now();
    while (output.find('\n') == std::string::npos && Clock::now() - started < std::chrono::seconds(5))
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const int sleeper = std::stoi(output);
    CHECK(alive(sleeper));

    process.terminate();
    CHECK(!process.running());
    CHECK(Clock::now() - started < std::chrono::seconds(5)); // didn't wait out the sleep
    // The sleep went too (reaped by init once its group is signalled).
    const auto stopping = Clock::now();
    while (alive(sleeper) && Clock::now() - stopping < std::chrono::seconds(2))
    {
      std::ifstream stat("/proc/" + std::to_string(sleeper) + "/stat");
      std::string pid, name, state;
      if (!(stat >> pid >> name >> state) || state == "Z")
        break; // gone, or dead and waiting to be reaped
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::ifstream stat("/proc/" + std::to_string(sleeper) + "/stat");
    std::string pid, name, state;
    CHECK(!(stat >> pid >> name >> state) || state == "Z");
  }

  TEST("a process should finish as usual and report its exit code")
  {
    ChildProcess process;
    CHECK(process.start({"sh", "-c", "exit 3"}, [](std::string_view) {}));
    while (process.running())
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(process.exitCode() == 3);
  }

  TEST("a native build environment should run commands as they are, stopped by terminate()")
  {
    const BuildEnvironment environment = BuildEnvironment::current();
    const std::vector<std::string> args{"cmake", "--build", "x"};
    CHECK(environment.stoppableCommand(args) == args);
    CHECK(environment.stopCommand(1234).empty());
  }

  return testing::report("childProcessTests");
}
