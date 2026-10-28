#include "../../testHarness.h"

#include <engine/core/jobs/workerPool.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace sfs;

namespace
{
// Wait (briefly) until `done` holds.
template <typename Done>
bool waitFor(Done done)
{
  for (int i = 0; i < 500 && !done(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return done();
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
    CHECK(waitFor([&] { return ran == 50; }));
    CHECK(elsewhere);
    CHECK(waitFor([&] { return pool.pending() == 0u; }));
  }

  TEST("destroying the pool should finish running jobs and drop queued ones")
  {
    std::atomic<int> ran = 0;
    std::atomic<bool> started = false;
    {
      WorkerPool pool(1);
      pool.submit(
          [&]
          {
            started = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            ++ran;
          });
      for (int i = 0; i < 10; ++i)
        pool.submit([&] { ++ran; });
      CHECK(waitFor([&] { return started.load(); }));
    }
    CHECK(ran == 1); // the running one finished; the rest never started
  }
}
