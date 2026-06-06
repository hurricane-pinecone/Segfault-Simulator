#include "engine/core/util/parallelFor.h"
#include "glm/glm/common.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sfs
{
namespace
{

using RangeFn =
    std::function<void(std::size_t, std::size_t, std::size_t)>;

// Below this many items the fork/join overhead outweighs the parallelism.
// (Unused on web, which always runs serially.)
[[maybe_unused]] constexpr std::size_t kSerialThreshold = 64;

unsigned int desiredWorkerThreads()
{
  const unsigned int hw = std::thread::hardware_concurrency();

  // Leave a core for the calling thread, which also runs a range.
  const unsigned int available = hw > 1 ? hw - 1 : 0;

#ifdef __EMSCRIPTEN__
  // Match -sPTHREAD_POOL_SIZE so we never exceed the pre-allocated worker pool.
  constexpr unsigned int kCap = 4;
#else
  // The per-edge shadow work is memory-bandwidth-bound, so more threads mainly
  // add contention. Cap low; tune against a trace.
  constexpr unsigned int kCap = 4;
#endif

  return glm::min(available, kCap);
}

// Persistent fork-join pool: worker `i` runs range `i`, the caller runs the
// last range, and run() blocks until every worker finishes.
class ForkJoinPool
{
public:
  static ForkJoinPool& instance()
  {
    static ForkJoinPool pool;
    return pool;
  }

  // Worker threads + the calling thread.
  std::size_t participants() const { return m_workers.size() + 1; }

  void run(std::size_t count, const RangeFn& fn)
  {
    const std::size_t p = participants();

    std::vector<std::pair<std::size_t, std::size_t>> ranges(p);
    const std::size_t base = count / p;
    const std::size_t remainder = count % p;

    std::size_t start = 0;
    for (std::size_t i = 0; i < p; i++)
    {
      const std::size_t n = base + (i < remainder ? 1 : 0);
      ranges[i] = {start, start + n};
      start += n;
    }

    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_fn = &fn;
      m_ranges = &ranges;
      m_pending = m_workers.size();
      m_generation++;
    }
    m_startCv.notify_all();

    // The caller runs the final range itself.
    const auto& own = ranges[p - 1];
    if (own.first < own.second)
      fn(own.first, own.second, p - 1);

    std::unique_lock<std::mutex> lock(m_mutex);
    m_doneCv.wait(lock, [this] { return m_pending == 0; });

    // Safe to drop now that every worker has finished calling *m_fn.
    m_fn = nullptr;
    m_ranges = nullptr;
  }

private:
  ForkJoinPool()
  {
    const unsigned int workers = desiredWorkerThreads();
    m_workers.reserve(workers);

    for (unsigned int i = 0; i < workers; i++)
      m_workers.emplace_back([this, i] { workerLoop(i); });
  }

  ~ForkJoinPool()
  {
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_shutdown = true;
    }
    m_startCv.notify_all();

    for (std::thread& worker : m_workers)
      if (worker.joinable())
        worker.join();
  }

  void workerLoop(std::size_t index)
  {
    std::size_t seenGeneration = 0;

    while (true)
    {
      const RangeFn* fn = nullptr;
      std::pair<std::size_t, std::size_t> range{0, 0};

      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_startCv.wait(lock,
                       [this, &seenGeneration]
                       { return m_shutdown || m_generation != seenGeneration; });

        if (m_shutdown)
          return;

        seenGeneration = m_generation;
        fn = m_fn;
        range = (*m_ranges)[index];
      }

      // *fn stays valid until run() observes m_pending == 0, which is after
      // this call completes, so no copy of the std::function is needed.
      if (range.first < range.second)
        (*fn)(range.first, range.second, index);

      {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (--m_pending == 0)
          m_doneCv.notify_one();
      }
    }
  }

  std::vector<std::thread> m_workers;

  std::mutex m_mutex;
  std::condition_variable m_startCv;
  std::condition_variable m_doneCv;

  std::size_t m_generation = 0;
  std::size_t m_pending = 0;
  bool m_shutdown = false;

  const RangeFn* m_fn = nullptr;
  const std::vector<std::pair<std::size_t, std::size_t>>* m_ranges = nullptr;
};

} // namespace

std::size_t parallelForRangeCount(std::size_t count)
{
#ifdef __EMSCRIPTEN__
  // Run serially on web: the fork-join blocks the calling thread, and blocking
  // the browser's main thread deadlocks (a not-yet-started worker can't start
  // while the event loop is blocked). True web parallelism needs
  // -sPROXY_TO_PTHREAD so main runs on a worker.
  (void)count;
  return 1;
#else
  if (count < kSerialThreshold)
    return 1;

  return ForkJoinPool::instance().participants();
#endif
}

void parallelFor(std::size_t count, const RangeFn& fn)
{
  if (count == 0)
    return;

#ifdef __EMSCRIPTEN__
  fn(0, count, 0);
  return;
#else
  if (count < kSerialThreshold ||
      ForkJoinPool::instance().participants() <= 1)
  {
    fn(0, count, 0);
    return;
  }

  ForkJoinPool::instance().run(count, fn);
#endif
}

} // namespace sfs
