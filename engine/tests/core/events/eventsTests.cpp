#include "../../testHarness.h"

#include <engine/core/ecs/ecs.h>
#include <engine/core/events/events.h>
#include <engine/core/logger/logger.h>
#include <engine/core/util/allocationMetrics.h>
#include <engine/core/util/debugFeatures.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace sfs;

namespace
{

struct Ping
{
  int value = 0;
};

struct Named
{
  std::string text;
};

struct Gone
{
  Entity entity;
};

// Can be moved into place, but not assigned to.
struct Fixed
{
  const int id;
};

// One type per test that needs its log line (they're logged once per type).
struct OffThread
{
  int value = 0;
};
struct OffThreadReader
{
  int value = 0;
};
struct OffThreadRace
{
  int value = 0;
};
struct WhileRead
{
  int value = 0;
};
struct Missed
{
  int value = 0;
};
struct Logged
{
  int value = 0;
};
struct Flood
{
  int value = 0;
};

struct FromWorker
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

// The values a reader reads now.
std::vector<int> readValues(EventReader<Ping>& reader)
{
  std::vector<int> values;
  for (const Ping& ping : reader.read())
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
  TEST("a reader should read events in send order, each once")
  {
    Events events;
    EventReader<Ping> reader = events.reader<Ping>();
    CHECK(reader.read().empty());
    for (int i = 0; i < 5; ++i)
      events.send(Ping{i});
    CHECK((readValues(reader) == std::vector<int>{0, 1, 2, 3, 4}));
    CHECK(reader.read().empty());
    events.send(Ping{5});
    CHECK((readValues(reader) == std::vector<int>{5}));
    events.update();
    events.send(Ping{6});
    CHECK((readValues(reader) == std::vector<int>{6}));
  }

  TEST("two readers should each read every event")
  {
    Events events;
    EventReader<Ping> first = events.reader<Ping>();
    EventReader<Ping> second = events.reader<Ping>();
    events.send(Ping{1});
    events.send(Ping{2});
    CHECK((readValues(first) == std::vector<int>{1, 2}));
    events.send(Ping{3});
    events.update();
    events.send(Ping{4});
    CHECK((readValues(first) == std::vector<int>{3, 4}));
    CHECK((readValues(second) == std::vector<int>{1, 2, 3, 4}));
  }

  TEST("a reader made after sends should read the events kept")
  {
    Events events;
    events.send(Ping{1});
    events.update();
    events.send(Ping{2});
    EventReader<Ping> late = events.reader<Ping>();
    CHECK(late.unread() == 2u);
    CHECK((readValues(late) == std::vector<int>{1, 2}));
  }

  TEST("an event should be kept through one swap and dropped at the second")
  {
    Events events;
    EventReader<Ping> reader = events.reader<Ping>();
    events.send(Ping{1});
    events.update();
    CHECK(reader.unread() == 1u);
    events.update();
    CHECK(reader.unread() == 0u);
    CHECK(reader.read().empty());
    CHECK(events.reader<Ping>().read().empty());
  }

  TEST("a reader should go through the last frame's tail, then this frame's")
  {
    Events events;
    EventReader<Ping> reader = events.reader<Ping>();
    events.send(Ping{1});
    CHECK((readValues(reader) == std::vector<int>{1}));
    events.send(Ping{2});
    events.send(Ping{3});
    events.update();
    events.send(Ping{4});
    const EventRange<Ping> range = reader.read();
    CHECK(range.size() == 3u);
    std::vector<int> values;
    for (auto it = range.begin(); it != range.end(); ++it)
      values.push_back(it->value);
    CHECK((values == std::vector<int>{2, 3, 4}));
  }

  TEST("a reader that missed events should count them and log once, in "
       "every build")
  {
    Events events;
    EventReader<Missed> reader = events.reader<Missed>();
    LogCapture log;
    events.send(Missed{1});
    events.send(Missed{2});
    events.update();
    events.update();
    events.send(Missed{3});
    std::vector<int> values;
    for (const Missed& missed : reader.read())
      values.push_back(missed.value);
    CHECK((values == std::vector<int>{3}));
    std::vector<EventStats> stats;
    events.stats(stats);
    CHECK(stats.size() == 1u);
    if (!stats.empty())
      CHECK(stats[0].missed == 2u);

    events.send(Missed{4});
    events.update();
    events.update();
    CHECK(reader.read().empty());
    events.stats(stats);
    if (!stats.empty())
      CHECK(stats[0].missed == 3u);
    CHECK(log.count(Logger::Level::WARN, "missed 2 events") == 1);
    CHECK(log.count(Logger::Level::WARN, "missed 1 events") == 0);
  }

  TEST("unread should count without marking read, and skip mark all read")
  {
    Events events;
    EventReader<Ping> reader = events.reader<Ping>();
    events.send(Ping{1});
    events.send(Ping{2});
    CHECK(reader.unread() == 2u);
    CHECK(reader.unread() == 2u);
    reader.skip();
    CHECK(reader.unread() == 0u);
    events.send(Ping{3});
    CHECK((readValues(reader) == std::vector<int>{3}));
  }

  TEST("a loop that breaks early should still mark every event read")
  {
    Events events;
    EventReader<Ping> reader = events.reader<Ping>();
    for (int i = 0; i < 4; ++i)
      events.send(Ping{i});
    for (const Ping& ping : reader.read())
      if (ping.value == 1)
        break;
    CHECK(reader.unread() == 0u);
  }

  TEST("a default reader should read nothing")
  {
    EventReader<Ping> reader;
    CHECK(reader.read().empty());
    CHECK(reader.unread() == 0u);
    reader.skip();
    CHECK(sizeof(EventReader<Ping>) == 24u);
  }

  TEST("events should move in and keep what they hold")
  {
    Events events;
    EventReader<Named> reader = events.reader<Named>();
    Named named{std::string(100, 'x')};
    events.send(std::move(named));
    const Named copied{"copied"};
    events.send(copied);
    std::vector<std::string> texts;
    for (const Named& event : reader.read())
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
    EventReader<Fixed> reader = events.reader<Fixed>();
    EventSender<Fixed> sender = events.sender<Fixed>();
    events.send(Fixed{1});
    CHECK(sender.send(Fixed{2}));
    events.update();
    events.send(Fixed{3});
    std::vector<int> ids;
    for (const Fixed& fixed : reader.read())
      ids.push_back(fixed.id);
    CHECK((ids == std::vector<int>{1, 2, 3}));
  }

#ifndef NDEBUG
  TEST("sending a type while going through its events should log and end the "
       "loop (debug builds)")
  {
    Events events;
    EventReader<WhileRead> reader = events.reader<WhileRead>();
    for (int i = 0; i < 3; ++i)
      events.send(WhileRead{i});
    LogCapture log;
    int seen = 0;
    for (const WhileRead& event : reader.read())
    {
      ++seen;
      events.send(WhileRead{event.value + 10});
    }
    CHECK(seen == 1);
    CHECK(log.count(Logger::Level::ERROR, "was sent while a loop") == 1);
    CHECK(reader.unread() == 1u);
  }
#endif

  TEST("events naming destroyed or reused-id entities should read invalid")
  {
    Registry registry;
    Events& events = registry.events();
    EventReader<Gone> reader = events.reader<Gone>();
    const Entity doomed = registry.createEntity();
    registry.flushEntities();
    events.send(Gone{doomed});
    registry.destroyEntity(doomed);
    registry.flushEntities();
    events.update();
    const Entity reused = registry.createEntity();
    registry.flushEntities();
    CHECK(reused.getId() == doomed.getId());
    int read = 0;
    for (const Gone& gone : reader.read())
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
    EventReader<OffThread> reader = events.reader<OffThread>();
    LogCapture log;
    std::thread worker(
        [&]
        {
          events.send(OffThread{1});
          events.send(OffThread{2});
        });
    worker.join();
    events.update();
    CHECK(reader.unread() == 0u);
    CHECK(log.count(Logger::Level::ERROR, "::OffThread>() was called off") ==
          1);

    std::optional<EventReader<OffThreadReader>> made;
    std::thread asker([&] { made = events.reader<OffThreadReader>(); });
    asker.join();
    CHECK(log.count(Logger::Level::ERROR,
                    "::OffThreadReader>() was called off") == 1);
    if (made)
      CHECK(made->unread() == 0u);
  }

  TEST("a send off the owning thread while the owner updates should be "
       "refused without a data race")
  {
    Events events;
    EventReader<OffThreadRace> reader = events.reader<OffThreadRace>();
    LogCapture log;
    std::atomic<bool> stop{false};
    std::atomic<int> tries{0};
    std::thread worker(
        [&]
        {
          while (!stop.load())
          {
            events.send(OffThreadRace{1});
            tries.fetch_add(1);
          }
        });
    for (int frame = 0; frame < 2000 || tries.load() < 100; ++frame)
      events.update();
    stop.store(true);
    worker.join();
    events.update();
    CHECK(reader.unread() == 0u);
    CHECK(log.count(
              Logger::Level::ERROR, "::OffThreadRace>() was called off") == 1);
  }

  TEST("update should make the calling thread the owner")
  {
    Events events;
    std::thread other(
        [&]
        {
          events.update();
          EventReader<Ping> reader = events.reader<Ping>();
          events.send(Ping{7});
          CHECK(reader.unread() == 1u);
        });
    other.join();
    events.update();
    EventReader<Ping> reader = events.reader<Ping>();
    CHECK((readValues(reader) == std::vector<int>{7}));
  }

  TEST("4 threads' events through senders should all arrive at the next "
       "swap, each thread's in order, ahead of the main thread's")
  {
    constexpr int kThreads = 4;
    constexpr int kEach = 10000;
    Events events;
    EventReader<FromWorker> reader = events.reader<FromWorker>();
    const EventSender<FromWorker> sender = events.sender<FromWorker>();
    std::vector<std::thread> workers;
    std::vector<int> failed(kThreads, 0);
    for (int t = 0; t < kThreads; ++t)
      workers.emplace_back(
          [sender, t, &failed]() mutable
          {
            for (int i = 0; i < kEach; ++i)
              if (!sender.send(FromWorker{t, i}))
                ++failed[t];
          });
    for (std::thread& worker : workers)
      worker.join();
    CHECK(reader.unread() == 0u);
    std::vector<EventStats> stats;
    events.stats(stats);
    if (!stats.empty())
      CHECK(stats[0].staged == static_cast<std::size_t>(kThreads * kEach));

    events.update();
    events.send(FromWorker{-1, 0});
    std::vector<int> next(kThreads, 0);
    bool ordered = true;
    int count = 0;
    bool mainLast = false;
    for (const FromWorker& event : reader.read())
    {
      ++count;
      if (event.thread < 0)
      {
        mainLast = count == kThreads * kEach + 1;
        continue;
      }
      ordered = ordered && event.index == next[event.thread];
      ++next[event.thread];
    }
    CHECK(count == kThreads * kEach + 1);
    CHECK(ordered);
    CHECK(mainLast);
    for (int t = 0; t < kThreads; ++t)
      CHECK(failed[t] == 0);
  }

  TEST("senders sending while the owner swaps should lose nothing")
  {
    constexpr int kThreads = 4;
    constexpr int kEach = 5000;
    Events events;
    EventReader<FromWorker> reader = events.reader<FromWorker>();
    const EventSender<FromWorker> sender = events.sender<FromWorker>();
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
      workers.emplace_back(
          [sender, t]() mutable
          {
            for (int i = 0; i < kEach; ++i)
              sender.send(FromWorker{t, i});
          });
    std::vector<int> next(kThreads, 0);
    bool ordered = true;
    int count = 0;
    const auto drain = [&]
    {
      for (const FromWorker& event : reader.read())
      {
        ordered = ordered && event.index == next[event.thread];
        ++next[event.thread];
        ++count;
      }
    };
    while (count < kThreads * kEach && events.frame() < 1000000)
    {
      events.update();
      drain();
    }
    for (std::thread& worker : workers)
      worker.join();
    events.update();
    drain();
    CHECK(count == kThreads * kEach);
    CHECK(ordered);
  }

  TEST("a sender outliving its Events should return false")
  {
    EventSender<Ping> sender;
    CHECK(!sender.send(Ping{1}));
    {
      Events events;
      sender = events.sender<Ping>();
      CHECK(sender.send(Ping{1}));
    }
    CHECK(!sender.send(Ping{2}));
  }

  TEST("stats should count each type's sent, kept, staged and capacity")
  {
    Events events;
    EventReader<Ping> reader = events.reader<Ping>();
    EventSender<Ping> sender = events.sender<Ping>();
    events.send(Ping{1});
    events.send(Ping{2});
    events.update();
    events.send(Ping{3});
    sender.send(Ping{4});
    std::vector<EventStats> stats;
    events.stats(stats);
    CHECK(stats.size() == 1u);
    if (!stats.empty())
    {
      CHECK(stats[0].type == "{anonymous}::Ping" ||
            stats[0].type.ends_with("Ping"));
      CHECK(stats[0].sentLastFrame == 2u);
      CHECK(stats[0].kept == 3u);
      CHECK(stats[0].staged == 1u);
      CHECK(stats[0].capacity >= 3u);
    }
  }

  TEST("logEvents should log the listed types' sends and workers' events")
  {
    debugFeatures::setLogEvents("Ping, Logged");
    CHECK(debugFeatures::logEvents("farm::Logged") == debugFeatures::enabled());
    CHECK(!debugFeatures::logEvents("farm::NotLogged"));
    CHECK(!debugFeatures::logEvents("Unlogged"));
    Events events;
    LogCapture log;
    EventSender<Logged> sender = events.sender<Logged>();
    events.send(Logged{1});
    sender.send(Logged{2});
    events.update();
    events.send(Flood{1});
    debugFeatures::setLogEvents("");
    if (debugFeatures::enabled())
    {
      CHECK(log.count(Logger::Level::INFO, "Logged #0 sent in frame 0") == 1);
      CHECK(log.count(Logger::Level::INFO, "sent by thread") == 1);
      CHECK(log.count(Logger::Level::INFO,
                      "#1 to #1 taken in from workers "
                      "in frame 1") == 1);
    }
    CHECK(log.count(Logger::Level::INFO, "Flood") == 0);
    debugFeatures::setLogEvents("*");
    CHECK(debugFeatures::logEvents("Anything") == debugFeatures::enabled());
    debugFeatures::setLogEvents("");
  }

  TEST("a queue past 65,536 kept events should warn once")
  {
    Events events;
    LogCapture log;
    for (int i = 0; i < 70000; ++i)
      events.send(Flood{i});
    events.update();
    if (debugFeatures::enabled())
      CHECK(log.count(Logger::Level::WARN, "Flood events are kept") == 1);
  }

  TEST("event type ids should be shared and distinct")
  {
    const int ping = detail::moduleTypeId<Ping, &detail::eventTypeId>();
    const int named = detail::moduleTypeId<Named, &detail::eventTypeId>();
    CHECK(ping != named);
    CHECK(detail::eventTypeId(detail::componentTypeKey<Ping>()) == ping);
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
    EventReader<Ping> first = events.reader<Ping>();
    EventReader<Ping> second = events.reader<Ping>();
    EventSender<Ping> sender = events.sender<Ping>();
    std::vector<EventStats> stats;
    long sum = 0;
    const auto frame = [&]
    {
      events.update();
      for (int i = 0; i < 64; ++i)
        events.send(Ping{i});
      for (int i = 0; i < 8; ++i)
        sender.send(Ping{i});
      for (const Ping& ping : first.read())
        sum += ping.value;
      for (const Ping& ping : second.read())
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
