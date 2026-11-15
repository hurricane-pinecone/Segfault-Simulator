#pragma once

#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sfs::nav
{

/**
 * THE planning scheduler: one worker pool every path request goes through
 * (NavigationSystem owns it; nav::Fleet, a game's own planning and the coarse
 * graph build all submit to it). Work is split into small tasks: a job is a
 * step function the scheduler calls again and again (one leg, one pass, one
 * layout candidate per call) until it says it's done, and between two steps
 * a worker may take up another job's, so one owner's long job never holds
 * another's short one up.
 *
 * Which task a free worker takes:
 * - Each job carries the time it's NEEDED BY (its deadline: now for a
 *   standing agent, else when it runs out of known path). A queued task that
 *   will miss it (now + its expected step time past the deadline) is
 *   ESCALATED: its deadline becomes now, it goes ahead of every task that
 *   isn't.
 * - Then a fair turn per owner (an agent): an owner with fewer tasks running
 *   goes first, and no owner holds more than (threads - reserve) workers at
 *   once, so another's short job always finds one free.
 * - Then earliest deadline first (then the order asked).
 * - Look-ahead jobs (the next leg of an agent with plenty of path left) are
 *   shed first under load: one with more than Settings::shedSlack seconds to
 *   spare isn't started while that would leave fewer than Settings::reserve
 *   workers free for what's asked next; it runs when the load drops or its
 *   deadline comes close (then, if it's going to miss, escalated as any).
 *
 * A job's legs are STREAMED: it publishes each one as it's ready, in order;
 * a published leg never changes. The consumer collects them on its own
 * thread (collect(): the legs since the last call). A leg published after
 * its job's deadline is a miss: counted, and logged with the real seconds it
 * was late (an agent stood that long at the end of what it had) once that's
 * over Settings::logLateAfter.
 *
 * A step may fork tasks that run in parallel (a field plan's layout
 * candidates): the job's next step runs once they've all finished. Errors
 * thrown by a step or a forked task are caught, logged and end the job
 * (Failed); nothing escapes into the game loop.
 */
class PlanScheduler
{
public:
  using Clock = std::chrono::steady_clock;
  using JobId = uint64_t;

  struct Settings
  {
    unsigned threads = 0;   // 0: the machine's hardware threads - 2, at least 2
    float shedSlack = 5.0f; // s to spare a look-ahead job may be deferred with
    unsigned reserve = 1;   // workers look-ahead work leaves free
    float logLateAfter =
        0.25f; // s late a miss is logged from (all are counted)
  };

  /** What a step is given: publish its legs, fork parallel tasks. */
  class Context
  {
  public:
    /** A leg, in order; it never changes once published. */
    void publish(std::any leg);
    /**
     * A task run in parallel with this job's others (any worker); the job's
     * next step waits for all of them. It's handed the job's cancel flag.
     */
    void fork(std::function<void(const std::atomic<bool>& cancel)> task);
    /** Cancelled: the step should end soon (a planner's PlanRequest::cancel).
     */
    const std::atomic<bool>& cancelFlag() const;
    bool cancelled() const;
    /** Legs published so far (this one's index next). */
    std::size_t published() const;

  private:
    friend class PlanScheduler;
    PlanScheduler* m_scheduler = nullptr;
    void* m_job = nullptr;
  };

  enum class Step
  {
    More, // call me again
    Done,
  };

  struct Request
  {
    uint64_t owner = 0; // whose (an agent's id): the fair turn is per owner
    std::string label;  // for the log ("Combine 1: travel")
    Clock::time_point deadline = Clock::now();
    bool lookAhead = false; // shed first under load
    float estimate = 0.05f; // s a step is expected to take (then measured)
    std::function<Step(Context&)> step;
  };

  enum class State
  {
    Unknown, // no such job (never was, cancelled, or finished and collected)
    Queued,
    Running,
    Done,
    Failed,
  };

  struct Collected
  {
    State state = State::Unknown;
    std::vector<std::any> legs; // published since the last collect()
  };

  struct Stats
  {
    uint64_t jobs = 0, steps = 0, forks = 0;
    uint64_t escalated = 0; // tasks moved to the top: going to miss
    uint64_t deferred = 0;  // look-ahead tasks held back under load (once each)
    uint64_t misses = 0;    // legs published after their deadline
    double latestMiss = 0.0; // s, the most late any leg was
  };

  PlanScheduler();
  explicit PlanScheduler(Settings settings);
  /** Cancels every job and waits for the running steps to end. */
  ~PlanScheduler();
  PlanScheduler(const PlanScheduler&) = delete;
  PlanScheduler& operator=(const PlanScheduler&) = delete;

  /** Start a job (thread-safe). */
  JobId submit(Request request);
  /** A one-step job publishing one leg (the result of `work`). */
  JobId submitOne(uint64_t owner,
                  std::string label,
                  Clock::time_point deadline,
                  std::function<std::any(const std::atomic<bool>& cancel)> work,
                  bool lookAhead = false);
  /**
   * The legs since the last call, and its state. A finished job (Done or
   * Failed) is forgotten once it's collected with nothing left.
   */
  Collected collect(JobId job);
  State state(JobId job) const;
  /** Drop it: its queued tasks never run, a running step is told to stop. */
  void cancel(JobId job);
  /**
   * Let go of something heavy without freeing it on this thread (the game
   * thread): a worker drops the last hold on it (a finished plan's copy of
   * the world, its checkers). Thread-safe. A job forgotten here (collected
   * finished, or cancelled) is let go the same way.
   */
  void release(std::shared_ptr<const void> object);
  /** Needed sooner (or later) than asked: an agent stopped early. */
  void setDeadline(JobId job, Clock::time_point deadline);

  unsigned threads() const { return static_cast<unsigned>(m_threads.size()); }
  /** Tasks queued or running. */
  std::size_t pending() const;
  Stats stats() const;
  const Settings& settings() const { return m_settings; }

  /** The pool size for this machine: hardware threads - 2, at least 2. */
  static unsigned defaultThreads();

private:
  struct Job;
  struct Task;
  void run();
  // The task a free worker takes now (null: none; `wake`: when to look
  // again for a deferred one). Under m_mutex.
  std::shared_ptr<Task> pick(Clock::time_point now, Clock::time_point& wake);
  void finishStep(const std::shared_ptr<Job>& job, bool more, double seconds);
  void publish(Job& job, std::any leg);
  void fork(Job& job, std::function<void(const std::atomic<bool>&)> task);
  void fail(Job& job, const std::string& what);

  Settings m_settings;
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::map<JobId, std::shared_ptr<Job>> m_jobs;
  std::vector<std::shared_ptr<Task>> m_queue;
  std::vector<std::shared_ptr<const void>> m_releases; // (release())
  std::map<uint64_t, unsigned> m_running;              // per owner
  std::size_t m_busy = 0;                              // workers running a task
  JobId m_next = 1;
  uint64_t m_order = 0;
  bool m_stopping = false;
  Stats m_stats;
  std::vector<std::thread> m_threads;
};

} // namespace sfs::nav
