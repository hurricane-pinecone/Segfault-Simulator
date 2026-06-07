#include "../../testHarness.h"

#include <engine/core/rendering/renderQueue.h>

#include <vector>

using namespace sfs;

namespace
{
// A base the queue holds, constructible from a distinct Derived, to exercise
// submitAll's subtype construction.
struct Derived
{
  int v = 0;
};
struct Base
{
  int v = 0;
  Base() = default;
  Base(int value) : v(value) {}
  Base(const Derived& d) : v(d.v) {}
};
} // namespace

int main()
{
  TEST("a fresh queue should be empty")
  {
    RenderQueue<int> q;
    CHECK(q.empty());
    CHECK(q.size() == 0);
    CHECK(q.items().empty());
  }

  TEST("submit should append items by copy and move")
  {
    RenderQueue<int> q;
    const int a = 1;
    q.submit(a); // copy
    q.submit(2); // move
    CHECK(q.size() == 2);
    CHECK(!q.empty());
    CHECK(q.items()[0] == 1);
    CHECK(q.items()[1] == 2);
  }

  TEST("clear should empty the queue")
  {
    RenderQueue<int> q;
    q.submit(1);
    q.submit(2);
    q.clear();
    CHECK(q.empty());
    CHECK(q.size() == 0);
  }

  TEST("mutableItems should expose the storage for in-place edits")
  {
    RenderQueue<int> q;
    q.submit(5);
    q.mutableItems()[0] = 9;
    CHECK(q.items()[0] == 9);
  }

  TEST("submitAll should append a whole vector")
  {
    RenderQueue<int> q;
    q.submit(1);
    q.submitAll(std::vector<int>{2, 3, 4});
    CHECK(q.size() == 4);
    CHECK(q.items()[3] == 4);
  }

  TEST("submitAll should construct the item type from a subtype")
  {
    RenderQueue<Base> q;
    q.submitAll(std::vector<Derived>{{10}, {20}});
    CHECK(q.size() == 2);
    CHECK(q.items()[0].v == 10); // built via Base(const Derived&)
    CHECK(q.items()[1].v == 20);
  }

  return testing::report("renderQueueTests");
}
