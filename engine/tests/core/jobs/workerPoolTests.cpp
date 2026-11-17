#include "../../testHarness.h"
#include "engine/core/jobs/workerPool.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace sfs;

namespace
{
// Waits until `done` holds. Something that never happens shows as a hang,
// which ctest's timeout reports.
template <typename Done>
void waitFor(Done done)
{
  while (!done())
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
} // namespace

int main()
{
  TEST("every job submitted should run, off the calling thread")
  {
    WorkerPool pool(3);
    std::atomic<int> ran = 0;
    std::atomic<bool> elsewhere = true;
    const std::thread::id caller = std::this_thread::get_id();

    for (int i = 0; i < 50; ++i)
      pool.submit(
          [&]
          {
            elsewhere = elsewhere && std::this_thread::get_id() != caller;
            ++ran;
          });

    waitFor([&] { return ran == 50; });
    CHECK(ran == 50);
    CHECK(elsewhere);
    waitFor([&] { return pool.pending() == 0u; });
  }

  TEST("destroying the pool should finish running jobs and drop queued ones")
  {
    std::atomic<int> ran = 0;
    std::atomic<bool> started = false;
    std::atomic<bool> queued = false;
    {
      WorkerPool pool(1);
      // It runs till the pool's dropped the ten queued behind it, which it
      // does as it's destroyed: then only this one's left pending.
      pool.submit(
          [&]
          {
            started = true;
            waitFor([&] { return queued && pool.pending() == 1u; });
            ++ran;
          });

      for (int i = 0; i < 10; ++i)
        pool.submit([&] { ++ran; });

      queued = true;
      waitFor([&] { return started.load(); });
    }
    CHECK(ran == 1); // the running one finished; the rest never started
  }

  TEST("a job that throws should be logged, and the pool should carry on")
  {
    std::atomic<int> ran = 0;
    WorkerPool pool(1);
    pool.submit([] { throw std::runtime_error("broken job"); });
    pool.submit([] { throw 42; });
    pool.submit([&] { ++ran; });
    waitFor([&] { return pool.pending() == 0u; });
    CHECK(ran == 1);
  }

  return testing::report("workerPoolTests");
}
