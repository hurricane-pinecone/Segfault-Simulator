#include "engine/core/util/asyncJobQueue.h"

#include <utility>

namespace sfs
{

AsyncJobQueue::AsyncJobQueue(unsigned threadCount)
{
#ifndef __EMSCRIPTEN__
  for (unsigned i = 0; i < threadCount; ++i)
    m_threads.emplace_back([this] { workerLoop(); });
#else
  (void)threadCount; // web: no threads, jobs run synchronously in submit()
#endif
}

AsyncJobQueue::~AsyncJobQueue()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = true;
  }
  m_cv.notify_all();
  for (std::thread& t : m_threads)
    if (t.joinable())
      t.join();
}

void AsyncJobQueue::submit(std::function<void()> job)
{
#ifdef __EMSCRIPTEN__
  job(); // no worker threads on web -- run inline
#else
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.push(std::move(job));
  }
  m_cv.notify_one();
#endif
}

std::size_t AsyncJobQueue::pending() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_jobs.size();
}

void AsyncJobQueue::workerLoop()
{
  for (;;)
  {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
      if (m_stop && m_jobs.empty())
        return;
      job = std::move(m_jobs.front());
      m_jobs.pop();
    }
    job();
  }
}

} // namespace sfs
