#include "../../testHarness.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/events/eventBroadcast.h"
#include "engine/core/events/eventQueue.h"
#include "engine/core/events/eventRows.h"
#include "engine/core/events/events.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/allocationMetrics.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/core/util/typeKey.h"

#include <atomic>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace sfs;

namespace
{

struct [[sfs::event_news]] PingEvent
{
  int value = 0;
};

struct [[sfs::event_news]] NamedEvent
{
  std::string text;
};

struct [[sfs::event_news]] GoneEvent
{
  Entity entity;
};

// Can be moved into place, but not assigned to.
struct [[sfs::event_news]] FixedEvent
{
  const int id;
};

// One type per test that needs its log line (they're logged once per type).
struct [[sfs::event_news]] OffThreadEvent
{
  int value = 0;
};
struct [[sfs::event_news]] OffThreadConsumerEvent
{
  int value = 0;
};
struct [[sfs::event_news]] OffThreadRaceEvent
{
  int value = 0;
};
struct [[sfs::event_news]] WhileReadEvent
{
  int value = 0;
};
struct [[sfs::event_news]] MissedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] LoggedEvent
{
  int value = 0;
};
struct [[sfs::event_news]] IdleRangeEvent
{
  int value = 0;
};
struct [[sfs::event_news]] KeptRangeEvent
{
  int value = 0;
};
struct [[sfs::event_news]] FloodEvent
{
  int value = 0;
};

struct [[sfs::event_news]] FromWorkerEvent
{
  int thread = 0;
  int index = 0;
};

// Collects what is logged while it lives, at each level (the log's repeat
// folding is reset first, so a message logged earlier still reaches it).
struct LogCapture
{
  std::vector<std::pair<Logger::Level, std::string>> lines;
  Logger::SinkId sink;

  LogCapture()
  {
    Logger::flush();
    sink =
        Logger::addSink([this](Logger::Level level, const std::string& message)
                        { lines.emplace_back(level, message); });
  }
  ~LogCapture()
  {
    Logger::removeSink(sink);
    Logger::flush();
  }
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;

  // How many lines at `level` say `part`.
  int count(Logger::Level level, std::string_view part) const
  {
    int found = 0;

    for (const auto& [at, line] : lines)
      if (at == level && line.find(part) != std::string::npos)
        ++found;

    return found;
  }
};

// The values a consumer reads now.
std::vector<int> readValues(EventConsumer<PingEvent>& consumer)
{
  std::vector<int> values;

  for (const PingEvent& ping : consumer.read())
    values.push_back(ping.value);

  return values;
}

// Whether allocations are counted in this build (a static engine build).
bool tracking()
{
  MemoryMetrics& metrics = getMemoryMetrics();
  const std::size_t probe = metrics.allocationCount.load();
  {
    ScopedMemoryTracking track(MemoryTrackingPhase::Update);
    int* volatile block = new int(1);
    delete block;
  }
  return metrics.allocationCount.load() != probe;
}

} // namespace

int main()
{
  TEST("a consumer should read events in send order, each once")
  {
    Events events;
    EventConsumer<PingEvent> consumer = events.consumer<PingEvent>();
    CHECK(consumer.read().empty());

    for (int i = 0; i < 5; ++i)
      events.send(PingEvent{i});

    CHECK((readValues(consumer) == std::vector<int>{0, 1, 2, 3, 4}));
    CHECK(consumer.read().empty());
    events.send(PingEvent{5});
    CHECK((readValues(consumer) == std::vector<int>{5}));
    events.update();
    events.send(PingEvent{6});
    CHECK((readValues(consumer) == std::vector<int>{6}));
  }

  TEST("two consumers should each read every event")
  {
    Events events;
    EventConsumer<PingEvent> first = events.consumer<PingEvent>();
    EventConsumer<PingEvent> second = events.consumer<PingEvent>();
    events.send(PingEvent{1});
    events.send(PingEvent{2});
    CHECK((readValues(first) == std::vector<int>{1, 2}));
    events.send(PingEvent{3});
    events.update();
    events.send(PingEvent{4});
    CHECK((readValues(first) == std::vector<int>{3, 4}));
    CHECK((readValues(second) == std::vector<int>{1, 2, 3, 4}));
  }

  TEST("a consumer made after sends should read the events kept")
  {
    Events events;
    events.send(PingEvent{1});
    events.update();
    events.send(PingEvent{2});
    EventConsumer<PingEvent> late = events.consumer<PingEvent>();
    CHECK(late.unread() == 2u);
    CHECK((readValues(late) == std::vector<int>{1, 2}));
  }

  TEST("an event should be kept until every consumer has read it")
  {
    Events events;
    EventConsumer<PingEvent> first = events.consumer<PingEvent>();
    EventConsumer<PingEvent> second = events.consumer<PingEvent>();
    events.send(PingEvent{1});

    for (int i = 0; i < 5; ++i)
      events.update();

    CHECK(first.unread() == 1u);
    CHECK((readValues(first) == std::vector<int>{1}));
    events.update();
    CHECK(second.unread() == 1u);
    CHECK((readValues(second) == std::vector<int>{1}));
    events.update();
    // Every consumer has read it: a new consumer finds nothing kept.
    CHECK(events.consumer<PingEvent>().read().empty());
  }

  TEST("a type with no consumers should keep an event through one frame "
       "boundary and drop it at the second")
  {
    Events events;
    events.send(PingEvent{1});
    events.update();
    EventConsumer<PingEvent> late = events.consumer<PingEvent>();
    CHECK(late.unread() == 1u);
    late = {};
    events.update();
    CHECK(events.consumer<PingEvent>().read().empty());
  }

  TEST("a consumer should deregister as it goes, and a moved one carry on")
  {
    Events events;
    std::vector<EventStats> stats;
    {
      EventConsumer<PingEvent> consumer = events.consumer<PingEvent>();
      events.send(PingEvent{1});
      events.update();
      events.update();
      events.stats(stats);
      CHECK((stats.size() == 1u && stats[0].kept == 1u &&
             stats[0].consumers == 1u));
      EventConsumer<PingEvent> moved = std::move(consumer);
      CHECK(consumer.read().empty());
      CHECK((readValues(moved) == std::vector<int>{1}));
      events.send(PingEvent{2});
    }
    events.update();
    events.stats(stats);
    CHECK((stats.size() == 1u && stats[0].consumers == 0u));
    events.update();
    events.stats(stats);
    CHECK((stats.size() == 1u && stats[0].kept == 0u));
  }

  TEST("a consumer should go through the last frame's events, then this "
       "frame's, as one range")
  {
    Events events;
    EventConsumer<PingEvent> consumer = events.consumer<PingEvent>();
    events.send(PingEvent{1});
    CHECK((readValues(consumer) == std::vector<int>{1}));
    events.send(PingEvent{2});
    events.send(PingEvent{3});
    events.update();
    events.send(PingEvent{4});
    const EventRange<PingEvent> range = consumer.read();
    CHECK(range.size() == 3u);
    std::vector<int> values;

    for (auto it = range.begin(); it != range.end(); ++it)
      values.push_back(it->value);

    CHECK((values == std::vector<int>{2, 3, 4}));
  }

  TEST("a consumer that never reads should be dropped past the cap, named, "
       "and count its misses once it reads, in every build")
  {
    Events events;
    events.setMaxKept(2);
    EventConsumer<MissedEvent> consumer = events.consumer<MissedEvent>();
    LogCapture log;

    for (int i = 1; i <= 5; ++i)
      events.send(MissedEvent{i});

    events.update();
    CHECK(log.count(Logger::Level::ERROR,
                    "so its oldest 3 were dropped. They were held by a "
                    "consumer") == 1);
    std::vector<int> values;

    for (const MissedEvent& missed : consumer.read())
      values.push_back(missed.value);

    CHECK((values == std::vector<int>{4, 5}));
    std::vector<EventStats> stats;
    events.stats(stats);
    CHECK(stats.size() == 1u);

    if (!stats.empty())
      CHECK(stats[0].missed == 3u);

    CHECK(log.count(Logger::Level::WARN, "a consumer missed 3") == 1);
  }

  TEST("a consumer outliving its events should read nothing, and let go "
       "safely")
  {
    EventConsumer<PingEvent> consumer;
    {
      Events events;
      consumer = events.consumer<PingEvent>();
      events.send(PingEvent{1});
    }
    CHECK(consumer.read().empty());
    CHECK(consumer.unread() == 0u);
    consumer.skip();
  }

  TEST("unread should count without marking read, and skip mark all read")
  {
    Events events;
    EventConsumer<PingEvent> consumer = events.consumer<PingEvent>();
    events.send(PingEvent{1});
    events.send(PingEvent{2});
    CHECK(consumer.unread() == 2u);
    CHECK(consumer.unread() == 2u);
    consumer.skip();
    CHECK(consumer.unread() == 0u);
    events.send(PingEvent{3});
    CHECK((readValues(consumer) == std::vector<int>{3}));
  }

  TEST("a loop that breaks early should still mark every event read")
  {
    Events events;
    EventConsumer<PingEvent> consumer = events.consumer<PingEvent>();

    for (int i = 0; i < 4; ++i)
      events.send(PingEvent{i});

    for (const PingEvent& ping : consumer.read())
      if (ping.value == 1)
        break;

    CHECK(consumer.unread() == 0u);
  }

  TEST("a default consumer should read nothing")
  {
    EventConsumer<PingEvent> consumer;
    CHECK(consumer.read().empty());
    CHECK(consumer.unread() == 0u);
    consumer.skip();
    CHECK(sizeof(EventConsumer<PingEvent>) == 32u);
  }

  TEST("events should move in and keep what they hold")
  {
    Events events;
    EventConsumer<NamedEvent> consumer = events.consumer<NamedEvent>();
    NamedEvent named{std::string(100, 'x')};
    events.send(std::move(named));
    const NamedEvent copied{"copied"};
    events.send(copied);
    std::vector<std::string> texts;

    for (const NamedEvent& event : consumer.read())
      texts.push_back(event.text);

    CHECK(texts.size() == 2u);

    if (texts.size() == 2u)
    {
      CHECK(texts[0] == std::string(100, 'x'));
      CHECK(texts[1] == "copied");
    }
  }

  TEST("an event type that can't be assigned should still be sent and "
       "staged")
  {
    Events events;
    EventConsumer<FixedEvent> consumer = events.consumer<FixedEvent>();
    EventBroadcast<FixedEvent> broadcast = events.broadcast<FixedEvent>();
    events.send(FixedEvent{1});
    CHECK(broadcast.send(FixedEvent{2}));
    events.update();
    events.send(FixedEvent{3});
    std::vector<int> ids;

    for (const FixedEvent& fixed : consumer.read())
      ids.push_back(fixed.id);

    CHECK((ids == std::vector<int>{1, 2, 3}));
  }

  TEST("sending a type while going through its events should keep the rows "
       "where they are and give the new ones next")
  {
    Events events;
    EventConsumer<WhileReadEvent> consumer = events.consumer<WhileReadEvent>();

    for (int i = 0; i < 3; ++i)
      events.send(WhileReadEvent{i});

    std::vector<int> seen;

    for (const WhileReadEvent& event : consumer.read())
    {
      // Enough sends to move a buffer that wasn't held still.
      for (int i = 0; i < 64; ++i)
        events.send(WhileReadEvent{100 + event.value});

      seen.push_back(event.value);
    }

    CHECK((seen == std::vector<int>{0, 1, 2}));
    CHECK(consumer.unread() == 192u);
    std::vector<int> next;

    for (const WhileReadEvent& event : consumer.read())
      next.push_back(event.value);

    CHECK(next.size() == 192u);
    CHECK((next.size() == 192u && next.front() == 100 && next[64] == 101 &&
           next.back() == 102));
  }

  TEST("a range kept past its frame should be warned about once, count what "
       "waits behind it, and let it all through when it goes")
  {
    Events events;
    events.setMaxKept(8);
    EventConsumer<KeptRangeEvent> keeper = events.consumer<KeptRangeEvent>();
    EventConsumer<KeptRangeEvent> other = events.consumer<KeptRangeEvent>();
    events.send(KeptRangeEvent{0});
    LogCapture log;
    {
      std::optional<EventRange<KeptRangeEvent>> kept(keeper.read());

      for (int frame = 0; frame < 5; ++frame)
      {
        events.send(KeptRangeEvent{frame + 1});
        events.send(KeptRangeEvent{frame + 1});
        events.update();
      }

      std::vector<EventStats> stats;
      events.stats(stats);
      // The cap covers the buffer and the lane together.
      CHECK((stats.size() == 1u && stats[0].kept == 8u));
      events.update();
      CHECK(log.count(Logger::Level::WARN, "was kept past its frame") == 1);
      CHECK(log.count(Logger::Level::ERROR, "behind a range kept past") == 1);
    }
    // The cap left the first event and the newest 7 sent behind the range.
    CHECK(other.read().size() == 8u);
  }

  TEST("a range first taken on an idle queue and taken again each frame "
       "should still be warned about and capped, workers' events with it")
  {
    Events events;
    events.setMaxKept(16);
    EventConsumer<IdleRangeEvent> keeper = events.consumer<IdleRangeEvent>();
    EventConsumer<IdleRangeEvent> other = events.consumer<IdleRangeEvent>();
    EventBroadcast<IdleRangeEvent> broadcast =
        events.broadcast<IdleRangeEvent>();
    LogCapture log;
    {
      // A member re-read each update: the new range begins before the old
      // one ends, so a range is always alive.
      EventRange<IdleRangeEvent> last = keeper.read();

      for (int frame = 0; frame < 20; ++frame)
      {
        events.send(IdleRangeEvent{frame});
        std::thread worker([&] { broadcast.send(IdleRangeEvent{-frame}); });
        worker.join();
        events.update();
        last = keeper.read();
      }

      std::vector<EventStats> stats;
      events.stats(stats);
      CHECK((stats.size() == 1u && stats[0].kept == 16u));
      CHECK((stats.size() == 1u && stats[0].staged == 0u));
      CHECK(log.count(Logger::Level::WARN, "was kept past its frame") == 1);
      CHECK(log.count(Logger::Level::ERROR, "behind a range kept past") == 1);
    }
    CHECK(other.read().size() == 16u);
  }

  TEST("events naming destroyed or reused-id entities should read invalid")
  {
    Registry registry;
    Events& events = registry.events();
    EventConsumer<GoneEvent> consumer = events.consumer<GoneEvent>();
    const Entity doomed = registry.createEntity();
    registry.flushEntities();
    events.send(GoneEvent{doomed});
    registry.destroyEntity(doomed);
    registry.flushEntities();
    events.update();
    const Entity reused = registry.createEntity();
    registry.flushEntities();
    CHECK(reused.getId() == doomed.getId());
    int read = 0;

    for (const GoneEvent& gone : consumer.read())
    {
      ++read;
      CHECK(!gone.entity.isValid());
      CHECK(!gone.entity);
    }

    CHECK(read == 1);
  }

  TEST("a send off the owning thread should be logged once and dropped")
  {
    Events events;
    EventConsumer<OffThreadEvent> consumer = events.consumer<OffThreadEvent>();
    LogCapture log;
    std::thread worker(
        [&]
        {
          events.send(OffThreadEvent{1});
          events.send(OffThreadEvent{2});
        });
    worker.join();
    events.update();
    CHECK(consumer.unread() == 0u);
    CHECK(log.count(
              Logger::Level::ERROR, "::OffThreadEvent>() was called off") == 1);

    std::optional<EventConsumer<OffThreadConsumerEvent>> made;
    std::thread asker([&]
                      { made = events.consumer<OffThreadConsumerEvent>(); });
    asker.join();
    CHECK(log.count(Logger::Level::ERROR,
                    "::OffThreadConsumerEvent>() was called off") == 1);

    if (made)
      CHECK(made->unread() == 0u);
  }

  TEST("a send off the owning thread while the owner updates should be "
       "refused without a data race")
  {
    Events events;
    EventConsumer<OffThreadRaceEvent> consumer =
        events.consumer<OffThreadRaceEvent>();
    LogCapture log;
    std::atomic<bool> stop{false};
    std::atomic<int> tries{0};
    std::thread worker(
        [&]
        {
          while (!stop.load())
          {
            events.send(OffThreadRaceEvent{1});
            tries.fetch_add(1);
          }
        });

    for (int frame = 0; frame < 2000 || tries.load() < 100; ++frame)
      events.update();

    stop.store(true);
    worker.join();
    events.update();
    CHECK(consumer.unread() == 0u);
    CHECK(log.count(Logger::Level::ERROR,
                    "::OffThreadRaceEvent>() was called off") == 1);
  }

  TEST("update should make the calling thread the owner")
  {
    Events events;
    std::thread other(
        [&]
        {
          events.update();
          EventConsumer<PingEvent> consumer = events.consumer<PingEvent>();
          events.send(PingEvent{7});
          CHECK(consumer.unread() == 1u);
        });
    other.join();
    events.update();
    EventConsumer<PingEvent> consumer = events.consumer<PingEvent>();
    CHECK((readValues(consumer) == std::vector<int>{7}));
  }

  TEST("4 threads' events through broadcasts should all arrive at the next "
       "swap, each thread's in order, ahead of the main thread's")
  {
    constexpr int THREADS = 4;
    constexpr int EACH = 10000;
    Events events;
    EventConsumer<FromWorkerEvent> consumer =
        events.consumer<FromWorkerEvent>();
    const EventBroadcast<FromWorkerEvent> broadcast =
        events.broadcast<FromWorkerEvent>();
    std::vector<std::thread> workers;
    std::vector<int> failed(THREADS, 0);

    for (int t = 0; t < THREADS; ++t)
      workers.emplace_back(
          [broadcast, t, &failed]() mutable
          {
            for (int i = 0; i < EACH; ++i)
              if (!broadcast.send(FromWorkerEvent{t, i}))
                ++failed[t];
          });

    for (std::thread& worker : workers)
      worker.join();

    CHECK(consumer.unread() == 0u);
    std::vector<EventStats> stats;
    events.stats(stats);

    if (!stats.empty())
      CHECK(stats[0].staged == static_cast<std::size_t>(THREADS * EACH));

    events.update();
    events.send(FromWorkerEvent{-1, 0});
    std::vector<int> next(THREADS, 0);
    bool ordered = true;
    int count = 0;
    bool mainLast = false;

    for (const FromWorkerEvent& event : consumer.read())
    {
      ++count;

      if (event.thread < 0)
      {
        mainLast = count == THREADS * EACH + 1;
        continue;
      }

      ordered = ordered && event.index == next[event.thread];
      ++next[event.thread];
    }

    CHECK(count == THREADS * EACH + 1);
    CHECK(ordered);
    CHECK(mainLast);

    for (int t = 0; t < THREADS; ++t)
      CHECK(failed[t] == 0);
  }

  TEST("broadcasts sending while the owner swaps should lose nothing")
  {
    constexpr int THREADS = 4;
    constexpr int EACH = 5000;
    Events events;
    EventConsumer<FromWorkerEvent> consumer =
        events.consumer<FromWorkerEvent>();
    const EventBroadcast<FromWorkerEvent> broadcast =
        events.broadcast<FromWorkerEvent>();
    std::vector<std::thread> workers;

    for (int t = 0; t < THREADS; ++t)
      workers.emplace_back(
          [broadcast, t]() mutable
          {
            for (int i = 0; i < EACH; ++i)
              broadcast.send(FromWorkerEvent{t, i});
          });

    std::vector<int> next(THREADS, 0);
    bool ordered = true;
    int count = 0;
    const auto drain = [&]
    {
      for (const FromWorkerEvent& event : consumer.read())
      {
        ordered = ordered && event.index == next[event.thread];
        ++next[event.thread];
        ++count;
      }
    };

    while (count < THREADS * EACH && events.frame() < 1000000)
    {
      events.update();
      drain();
    }

    for (std::thread& worker : workers)
      worker.join();

    events.update();
    drain();
    CHECK(count == THREADS * EACH);
    CHECK(ordered);
  }

  TEST("a broadcast outliving its Events should return false")
  {
    EventBroadcast<PingEvent> broadcast;
    CHECK(!broadcast.send(PingEvent{1}));
    {
      Events events;
      broadcast = events.broadcast<PingEvent>();
      CHECK(broadcast.send(PingEvent{1}));
    }
    CHECK(!broadcast.send(PingEvent{2}));
  }

  TEST("stats should count each type's sent, kept, staged and capacity")
  {
    Events events;
    EventConsumer<PingEvent> consumer = events.consumer<PingEvent>();
    EventBroadcast<PingEvent> broadcast = events.broadcast<PingEvent>();
    events.send(PingEvent{1});
    events.send(PingEvent{2});
    events.update();
    events.send(PingEvent{3});
    broadcast.send(PingEvent{4});
    std::vector<EventStats> stats;
    events.stats(stats);
    CHECK(stats.size() == 1u);

    if (!stats.empty())
    {
      CHECK(stats[0].type == "{anonymous}::PingEvent" ||
            stats[0].type.ends_with("PingEvent"));
      CHECK(stats[0].sentLastFrame == 2u);
      CHECK(stats[0].kept == 3u);
      CHECK(stats[0].staged == 1u);
      CHECK(stats[0].capacity >= 3u);
    }
  }

  TEST("logEvents should log the listed types' sends and workers' events")
  {
    debugFeatures::setLogEvents("PingEvent, LoggedEvent");
    CHECK(debugFeatures::logEvents("game::LoggedEvent") ==
          debugFeatures::enabled());
    CHECK(!debugFeatures::logEvents("game::NotLogged"));
    CHECK(!debugFeatures::logEvents("Unlogged"));
    Events events;
    LogCapture log;
    EventBroadcast<LoggedEvent> broadcast = events.broadcast<LoggedEvent>();
    events.send(LoggedEvent{1});
    broadcast.send(LoggedEvent{2});
    events.update();
    events.send(FloodEvent{1});
    debugFeatures::setLogEvents("");

    if (debugFeatures::enabled())
    {
      CHECK(log.count(Logger::Level::INFO,
                      "LoggedEvent [news] #0 sent in frame 0") == 1);
      CHECK(log.count(Logger::Level::INFO, "sent by thread") == 1);
      CHECK(log.count(Logger::Level::INFO,
                      "#1 to #1 taken in from workers "
                      "in frame 1") == 1);
    }

    CHECK(log.count(Logger::Level::INFO, "FloodEvent") == 0);
    debugFeatures::setLogEvents("*");
    CHECK(debugFeatures::logEvents("Anything") == debugFeatures::enabled());
    debugFeatures::setLogEvents("");
  }

  TEST("a queue past 65,536 kept events should warn once, naming who holds "
       "them")
  {
    Events events;
    EventConsumer<FloodEvent> consumer = events.consumer<FloodEvent>();
    LogCapture log;

    for (int i = 0; i < 70000; ++i)
      events.send(FloodEvent{i});

    events.update();
    events.update();

    if (debugFeatures::enabled())
      CHECK(log.count(Logger::Level::WARN,
                      "FloodEvent events are kept, held by a consumer") == 1);
  }

  TEST("event type ids should be shared and distinct")
  {
    const int ping = detail::moduleTypeId<PingEvent, &detail::eventTypeId>();
    const int named = detail::moduleTypeId<NamedEvent, &detail::eventTypeId>();
    CHECK(ping != named);
    CHECK(detail::eventTypeId(detail::componentTypeKey<PingEvent>()) == ping);
  }

  TEST("steady traffic should allocate nothing once warm (static build)")
  {
    if (!tracking())
    {
      std::fprintf(stderr,
                   "      note: allocation tracking is off in this build; "
                   "nothing to count\n");
      continue;
    }

    Events events;
    EventConsumer<PingEvent> first = events.consumer<PingEvent>();
    EventConsumer<PingEvent> second = events.consumer<PingEvent>();
    EventBroadcast<PingEvent> broadcast = events.broadcast<PingEvent>();
    std::vector<EventStats> stats;
    long sum = 0;
    const auto frame = [&]
    {
      events.update();

      for (int i = 0; i < 64; ++i)
        events.send(PingEvent{i});

      for (int i = 0; i < 8; ++i)
        broadcast.send(PingEvent{i});

      for (const PingEvent& ping : first.read())
        sum += ping.value;

      for (const PingEvent& ping : second.read())
        sum += ping.value;

      events.stats(stats);
    };

    // Until both buffers (and the inbox's two vectors) have grown to a frame's
    // events, workers' included.
    for (int i = 0; i < 4; ++i)
      frame();

    MemoryMetrics& metrics = getMemoryMetrics();
    const std::size_t before = metrics.allocationCount.load();
    {
      ScopedMemoryTracking track(MemoryTrackingPhase::Update);

      for (int i = 0; i < 100; ++i)
        frame();
    }
    CHECK(metrics.allocationCount.load() == before);
    CHECK(sum > 0);
  }

  return testing::report("eventsTests");
}
