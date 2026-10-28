#include "engine/core/jobs/workerPool.h"

#include <algorithm>

namespace sfs
{

WorkerPool::WorkerPool(unsigned threads)
{
  for (unsigned i = 0; i < std::max(threads, 1u); ++i)
    m_threads.emplace_back([this] { run(); });
}

WorkerPool::~WorkerPool()
{
  {
    const std::lock_guard lock(m_mutex);
    m_stopping = true;
    m_queue.clear();
  }
  m_wake.notify_all();
  for (std::thread& thread : m_threads)
    thread.join();
}

void WorkerPool::submit(std::function<void()> job)
{
  {
    const std::lock_guard lock(m_mutex);
    m_queue.push_back(std::move(job));
  }
  m_wake.notify_one();
}

std::size_t WorkerPool::pending() const
{
  const std::lock_guard lock(m_mutex);
  return m_queue.size() + m_running;
}

void WorkerPool::run()
{
  for (;;)
  {
    std::function<void()> job;
    {
      std::unique_lock lock(m_mutex);
      m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
      if (m_stopping)
        return;
      job = std::move(m_queue.front());
      m_queue.pop_front();
      ++m_running;
    }
    job();
    const std::lock_guard lock(m_mutex);
    --m_running;
  }
}

} // namespace sfs
