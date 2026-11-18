#include "../../engine/tests/testHarness.h"
#include "build/buildEnvironment.h"
#include "build/childProcess.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
bool alive(int pid) { return ::kill(pid, 0) == 0; }
} // namespace

int main()
{
  TEST("terminate() should stop the process and what it started, without "
       "waiting for them")
  {
    // A shell that starts a sleep of days, says which, and waits on it: if
    // terminate() waited for it, the test would hang.
    std::string output;
    ChildProcess process;
    CHECK(process.start({"sh", "-c", "sleep 1000000 & echo $!; wait"},
                        [&](std::string_view chunk) { output += chunk; }));

    while (output.find('\n') == std::string::npos)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int sleeper = std::stoi(output);
    CHECK(alive(sleeper));

    process.terminate();
    CHECK(!process.running());

    // The sleep went too (reaped by init once its group is signalled).
    while (alive(sleeper))
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

  TEST("a process should get the variables it's given, over the editor's own")
  {
    ::setenv("SFS_CHILD_TEST_KEPT", "kept", 1);
    ::setenv("SFS_CHILD_TEST_REPLACED", "old", 1);

    std::string output;
    ChildProcess process;
    ChildProcess::Options options;
    options.environment = {
        {"SFS_CHILD_TEST_REPLACED", "new"}, {"SFS_CHILD_TEST_ADDED", "added"}};
    CHECK(process.start(
        {"sh",
         "-c",
         "echo \"$SFS_CHILD_TEST_KEPT $SFS_CHILD_TEST_REPLACED "
         "$SFS_CHILD_TEST_ADDED\""},
        [&](std::string_view chunk) { output += chunk; },
        std::move(options)));

    while (process.running())
      std::this_thread::sleep_for(std::chrono::milliseconds(5));

    CHECK(output == "kept new added\n");
    CHECK(std::string(std::getenv("SFS_CHILD_TEST_REPLACED")) == "old");
  }

  TEST("a native build environment should run commands as they are, stopped by "
       "terminate()")
  {
    const BuildEnvironment environment = BuildEnvironment::current();
    const std::vector<std::string> args{"cmake", "--build", "x"};
    CHECK(environment.stoppableCommand(args) == args);
    CHECK(environment.stopCommand(1234).empty());
  }

  return testing::report("childProcessTests");
}
