#include "engine/runtime/navigation/planScheduler.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/threadPriority.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <tuple>

namespace sfs::nav
{

namespace
{
double secondsBetween(PlanScheduler::Clock::time_point a,
                      PlanScheduler::Clock::time_point b)
{
  return std::chrono::duration<double>(b - a).count();
}

PlanScheduler::Clock::duration toDuration(double seconds)
{
  return std::chrono::duration_cast<PlanScheduler::Clock::duration>(
      std::chrono::duration<double>(seconds));
}
} // namespace

struct PlanScheduler::Job
{
  JobId id = 0;
  Request request;
  Clock::time_point asked;
  std::atomic<bool> cancel{false};
  State state = State::Queued;
  std::vector<std::any> legs;
  std::size_t collected = 0;
  unsigned forks = 0;     // forked tasks not finished
  bool stepWaits = false; // its next step waits for the forks
  bool doneWaits = false; // done once the forks are
  double estimate = 0.05; // s per step, measured as it goes
};

struct PlanScheduler::Task
{
  std::shared_ptr<Job> job;
  std::function<void(const std::atomic<bool>&)> fork; // empty: the job's step
  uint64_t order = 0;
  bool escalated = false;
  bool deferred = false;
};

void PlanScheduler::Context::publish(std::any leg)
{
  m_scheduler->publish(*static_cast<Job*>(m_job), std::move(leg));
}

void PlanScheduler::Context::fork(
    std::function<void(const std::atomic<bool>&)> task)
{
  m_scheduler->fork(*static_cast<Job*>(m_job), std::move(task));
}

const std::atomic<bool>& PlanScheduler::Context::cancelFlag() const
{
  return static_cast<Job*>(m_job)->cancel;
}

bool PlanScheduler::Context::cancelled() const
{
  return cancelFlag().load(std::memory_order_relaxed);
}

std::size_t PlanScheduler::Context::published() const
{
  const std::lock_guard lock(m_scheduler->m_mutex);
  return static_cast<Job*>(m_job)->legs.size();
}

unsigned PlanScheduler::defaultThreads()
{
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 4 ? hw - 2 : 2u;
}

PlanScheduler::PlanScheduler() : PlanScheduler(Settings{}) {}

PlanScheduler::PlanScheduler(Settings settings) : m_settings(settings)
{
  const unsigned n =
      m_settings.threads > 0 ? m_settings.threads : defaultThreads();
  for (unsigned i = 0; i < n; ++i)
    m_threads.emplace_back([this] { run(); });
}

PlanScheduler::~PlanScheduler()
{
  {
    const std::lock_guard lock(m_mutex);
    m_stopping = true;
    for (auto& [id, job] : m_jobs)
      job->cancel = true;
    m_queue.clear();
  }
  m_wake.notify_all();
  for (std::thread& t : m_threads)
    t.join();
}

PlanScheduler::JobId PlanScheduler::submit(Request request)
{
  auto job = std::make_shared<Job>();
  job->asked = Clock::now();
  job->estimate = std::max(request.estimate, 0.0f);
  job->request = std::move(request);
  if (!job->request.step)
  {
    LOG_ERROR("Planning: a job with nothing to do ('" + job->request.label +
              "'): dropped");
    return 0;
  }
  {
    const std::lock_guard lock(m_mutex);
    job->id = m_next++;
    m_jobs.emplace(job->id, job);
    auto task = std::make_shared<Task>();
    task->job = job;
    task->order = m_order++;
    m_queue.push_back(std::move(task));
    ++m_stats.jobs;
  }
  m_wake.notify_one();
  return job->id;
}

PlanScheduler::JobId
PlanScheduler::submitOne(uint64_t owner,
                         std::string label,
                         Clock::time_point deadline,
                         std::function<std::any(const std::atomic<bool>&)> work,
                         bool lookAhead)
{
  Request r;
  r.owner = owner;
  r.label = std::move(label);
  r.deadline = deadline;
  r.lookAhead = lookAhead;
  r.step = [work = std::move(work)](Context& ctx)
  {
    std::any leg = work(ctx.cancelFlag());
    if (!ctx.cancelled())
      ctx.publish(std::move(leg));
    return Step::Done;
  };
  return submit(std::move(r));
}

PlanScheduler::Collected PlanScheduler::collect(JobId id)
{
  Collected out;
  const std::lock_guard lock(m_mutex);
  const auto it = m_jobs.find(id);
  if (it == m_jobs.end())
    return out;
  Job& job = *it->second;
  out.state = job.state;
  for (std::size_t i = job.collected; i < job.legs.size(); ++i)
    out.legs.push_back(job.legs[i]); // (a copy: the published leg stays)
  job.collected = job.legs.size();
  if (job.state == State::Done || job.state == State::Failed)
  {
    // (Its step and what it holds let go on a worker: release().)
    m_releases.push_back(std::move(it->second));
    m_jobs.erase(it);
    m_wake.notify_one();
  }
  return out;
}

void PlanScheduler::release(std::shared_ptr<const void> object)
{
  if (!object)
    return;
  {
    const std::lock_guard lock(m_mutex);
    m_releases.push_back(std::move(object));
  }
  m_wake.notify_one();
}

PlanScheduler::State PlanScheduler::state(JobId id) const
{
  const std::lock_guard lock(m_mutex);
  const auto it = m_jobs.find(id);
  return it == m_jobs.end() ? State::Unknown : it->second->state;
}

void PlanScheduler::cancel(JobId id)
{
  const std::lock_guard lock(m_mutex);
  const auto it = m_jobs.find(id);
  if (it == m_jobs.end())
    return;
  it->second->cancel = true;
  m_queue.erase(std::remove_if(m_queue.begin(),
                               m_queue.end(),
                               [&](const std::shared_ptr<Task>& t)
                               { return t->job->id == id; }),
                m_queue.end());
  m_releases.push_back(std::move(it->second));
  m_jobs.erase(it);
  m_wake.notify_one();
}

void PlanScheduler::setDeadline(JobId id, Clock::time_point deadline)
{
  {
    const std::lock_guard lock(m_mutex);
    const auto it = m_jobs.find(id);
    if (it == m_jobs.end())
      return;
    it->second->request.deadline = deadline;
  }
  m_wake.notify_all();
}

std::size_t PlanScheduler::pending() const
{
  const std::lock_guard lock(m_mutex);
  return m_queue.size() + m_busy;
}

PlanScheduler::Stats PlanScheduler::stats() const
{
  const std::lock_guard lock(m_mutex);
  return m_stats;
}

std::shared_ptr<PlanScheduler::Task>
PlanScheduler::pick(Clock::time_point now, Clock::time_point& wake)
{
  wake = Clock::time_point::max();
  if (m_queue.empty())
    return nullptr;
  const std::size_t free = m_threads.size() - m_busy; // (this one included)
  const bool loaded = free <= m_settings.reserve;
  const std::size_t perOwner =
      std::max<std::size_t>(1,
                            m_threads.size() > m_settings.reserve
                                ? m_threads.size() - m_settings.reserve
                                : 1);
  std::size_t best = m_queue.size();
  using Key = std::tuple<bool, unsigned, Clock::time_point, uint64_t>;
  Key bestKey{true, ~0u, Clock::time_point::max(), ~uint64_t{0}};
  for (std::size_t i = 0; i < m_queue.size(); ++i)
  {
    Task& t = *m_queue[i];
    const Job& job = *t.job;
    const Clock::time_point deadline = job.request.deadline;
    // Going to miss: its deadline's now, ahead of everything that isn't.
    if (!t.escalated && now + toDuration(job.estimate) >= deadline)
    {
      t.escalated = true;
      ++m_stats.escalated;
    }
    // Look-ahead with plenty to spare, the pool loaded: deferred.
    if (job.request.lookAhead && !t.escalated && loaded)
    {
      const Clock::time_point due =
          deadline - toDuration(job.estimate + m_settings.shedSlack);
      if (now < due)
      {
        if (!t.deferred)
        {
          t.deferred = true;
          ++m_stats.deferred;
        }
        wake = std::min(wake, due);
        continue;
      }
    }
    const auto running = m_running.find(job.request.owner);
    const unsigned busy = running == m_running.end() ? 0u : running->second;
    // (One owner never holds every worker: another's short job always
    // finds one, however long this one's steps are.)
    if (busy >= perOwner)
      continue;
    const Key key{!t.escalated, busy, deadline, t.order};
    if (best == m_queue.size() || key < bestKey)
    {
      best = i;
      bestKey = key;
    }
  }
  if (best == m_queue.size())
    return nullptr;
  std::shared_ptr<Task> task = std::move(m_queue[best]);
  m_queue.erase(m_queue.begin() + static_cast<std::ptrdiff_t>(best));
  return task;
}

void PlanScheduler::run()
{
  // (Planning may take every core it's given, never the game's or the
  // renderer's turn: a game frame waiting on a planner's time slice is the
  // hitch this pool is there to avoid.)
  lowerThisThreadPriority();
  for (;;)
  {
    std::shared_ptr<Task> task;
    std::shared_ptr<const void> released;
    {
      std::unique_lock lock(m_mutex);
      for (;;)
      {
        if (m_stopping)
          return;
        if (!m_releases.empty())
        {
          released = std::move(m_releases.back());
          m_releases.pop_back();
          break;
        }
        Clock::time_point wake;
        task = pick(Clock::now(), wake);
        if (task)
          break;
        if (wake == Clock::time_point::max())
          m_wake.wait(lock);
        else
          m_wake.wait_until(lock, wake);
      }
      if (released)
      {
        lock.unlock();
        released.reset(); // (freed here, off the thread that let it go)
        continue;
      }
      ++m_busy;
      ++m_running[task->job->request.owner];
      if (task->job->state == State::Queued)
        task->job->state = State::Running;
      ++(task->fork ? m_stats.forks : m_stats.steps);
    }
    Job& job = *task->job;
    const Clock::time_point t0 = Clock::now();
    bool more = false;
    std::string failed;
    try
    {
      if (task->fork)
        task->fork(job.cancel);
      else if (!job.cancel)
      {
        Context ctx;
        ctx.m_scheduler = this;
        ctx.m_job = &job;
        more = job.request.step(ctx) == Step::More;
      }
    }
    catch (const std::exception& error)
    {
      failed = error.what();
    }
    catch (...)
    {
      failed = "unknown error";
    }
    const double seconds = secondsBetween(t0, Clock::now());
    if (!failed.empty())
      fail(job, failed);
    {
      const std::lock_guard lock(m_mutex);
      --m_busy;
      if (--m_running[job.request.owner] == 0)
        m_running.erase(job.request.owner);
      if (task->fork)
      {
        --job.forks;
        if (job.forks == 0 && !job.cancel && job.state != State::Failed)
        {
          if (job.stepWaits)
          {
            job.stepWaits = false;
            auto next = std::make_shared<Task>();
            next->job = task->job;
            next->order = m_order++;
            m_queue.push_back(std::move(next));
          }
          else if (job.doneWaits)
            job.state = State::Done;
        }
      }
      else
      {
        job.estimate = 0.5 * job.estimate + 0.5 * seconds;
        finishStep(task->job, more, seconds);
      }
    }
    m_wake.notify_all(); // (a freed worker: deferred work may go now)
  }
}

void PlanScheduler::finishStep(const std::shared_ptr<Job>& job,
                               bool more,
                               double)
{
  if (job->cancel || job->state == State::Failed)
    return;
  if (more)
  {
    if (job->forks > 0)
    {
      job->stepWaits = true;
      return;
    }
    auto next = std::make_shared<Task>();
    next->job = job;
    next->order = m_order++;
    m_queue.push_back(std::move(next));
    return;
  }
  if (job->forks > 0)
    job->doneWaits = true;
  else
    job->state = State::Done;
}

void PlanScheduler::publish(Job& job, std::any leg)
{
  const Clock::time_point now = Clock::now();
  double late = 0.0;
  std::size_t index = 0;
  {
    const std::lock_guard lock(m_mutex);
    if (job.cancel)
      return;
    job.legs.push_back(std::move(leg));
    index = job.legs.size();
    late = secondsBetween(job.request.deadline, now);
    if (late > 0.0)
    {
      ++m_stats.misses;
      m_stats.latestMiss = std::max(m_stats.latestMiss, late);
    }
  }
  if (late > m_settings.logLateAfter)
  {
    char text[160];
    std::snprintf(text,
                  sizeof(text),
                  "' leg %zu came %.2f s after it was needed (%.2f s after "
                  "it was asked)",
                  index,
                  late,
                  secondsBetween(job.asked, now));
    LOG_INFO("Planning: '" + job.request.label + text);
  }
}

void PlanScheduler::fork(Job& job,
                         std::function<void(const std::atomic<bool>&)> work)
{
  {
    const std::lock_guard lock(m_mutex);
    if (job.cancel || !work)
      return;
    const auto it = m_jobs.find(job.id);
    if (it == m_jobs.end())
      return;
    auto task = std::make_shared<Task>();
    task->job = it->second;
    task->fork = std::move(work);
    task->order = m_order++;
    ++job.forks;
    m_queue.push_back(std::move(task));
  }
  m_wake.notify_one();
}

void PlanScheduler::fail(Job& job, const std::string& what)
{
  LOG_ERROR("Planning: '" + job.request.label + "' failed: " + what);
  const std::lock_guard lock(m_mutex);
  job.state = State::Failed;
  job.cancel = true;
  const JobId id = job.id;
  m_queue.erase(std::remove_if(m_queue.begin(),
                               m_queue.end(),
                               [&](const std::shared_ptr<Task>& t)
                               { return t->job->id == id; }),
                m_queue.end());
}

} // namespace sfs::nav
