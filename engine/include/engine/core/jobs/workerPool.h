#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sfs
{

/**
 * A few threads that run jobs in the background, oldest first: slow work
 * (reading and decoding files, drawing previews) kept off the main thread.
 * A job hands its result back itself, e.g. into a queue the main thread
 * drains; nothing here touches the game or the GPU.
 *
 * Destroying the pool drops the jobs not yet started and waits for the
 * running ones to finish.
 */
class WorkerPool
{
public:
  explicit WorkerPool(unsigned threads = 2);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  void submit(std::function<void()> job);

  /** Jobs queued or running. */
  std::size_t pending() const;

private:
  void run();

  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::deque<std::function<void()>> m_queue;
  std::size_t m_running = 0;
  bool m_stopping = false;
  std::vector<std::thread> m_threads;
};

} // namespace sfs
