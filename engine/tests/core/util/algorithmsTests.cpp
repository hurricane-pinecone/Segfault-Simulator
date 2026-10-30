#include "../../testHarness.h"

#include <engine/core/util/algorithms/aabbSweep.h>

#include <vector>

using namespace sfs;

int main()
{
  TEST("sweepAabb should report the entry parameter and face normal on a hit")
  {
    float tEnter = 0.0f;
    glm::vec2 normal{0.0f, 0.0f};
    // segment from (-2,0) moving +X by 4 into box [-1,-1]..[1,1]
    const bool hit = sweepAabb({-2.0f, 0.0f},
                               {4.0f, 0.0f},
                               {-1.0f, -1.0f},
                               {1.0f, 1.0f},
                               tEnter,
                               normal);
    CHECK(hit);
    CHECK(testing::approx(tEnter, 0.25f)); // enters the low x face at t=0.25
    CHECK(testing::approx(normal.x, -1.0f));
    CHECK(testing::approx(normal.y, 0.0f));
  }

  TEST("sweepAabb should miss a segment running alongside the box")
  {
    float tEnter = 0.0f;
    glm::vec2 normal{0.0f, 0.0f};
    const bool hit = sweepAabb({-2.0f, 5.0f},
                               {4.0f, 0.0f},
                               {-1.0f, -1.0f},
                               {1.0f, 1.0f},
                               tEnter,
                               normal);
    CHECK(!hit);
  }

  TEST("sweepAabb should not report a hit when the segment starts inside")
  {
    float tEnter = 0.0f;
    glm::vec2 normal{0.0f, 0.0f};
    const bool hit = sweepAabb({0.0f, 0.0f},
                               {4.0f, 0.0f},
                               {-1.0f, -1.0f},
                               {1.0f, 1.0f},
                               tEnter,
                               normal);
    CHECK(!hit);
  }

  return testing::report("algorithmsTests");
}
