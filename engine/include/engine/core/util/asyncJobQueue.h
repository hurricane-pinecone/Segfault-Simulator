#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace sfs
{

// A small persistent background thread pool that runs submitted jobs off the
// caller's thread (distinct from the fork-join parallelFor, which blocks the
// caller). For latency-hiding work that spans frames -- chunk generation,
// meshing -- where the main thread submits and later drains results. Each owner
// holds its own queue and joins its workers on destruction, so a job that
// captures the owner never outlives it (declare the queue as the LAST member so
// it tears down first). On web (no threads) jobs run synchronously in submit().
class AsyncJobQueue
{
public:
  explicit AsyncJobQueue(unsigned threadCount);
  ~AsyncJobQueue();

  AsyncJobQueue(const AsyncJobQueue&) = delete;
  AsyncJobQueue& operator=(const AsyncJobQueue&) = delete;

  void submit(std::function<void()> job);
  std::size_t pending() const;

private:
  void workerLoop();

  std::vector<std::thread> m_threads;
  std::queue<std::function<void()>> m_jobs;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_stop = false;
};

} // namespace sfs
