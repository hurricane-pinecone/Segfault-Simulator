#include "../../testHarness.h"

#include <engine/core/util/algorithms/aabbSweep.h>
#include <engine/core/util/algorithms/gridDDA.h>
#include <engine/core/util/algorithms/polygonClip.h>

#include <vector>

using namespace sfs;

int main()
{
  // --- sweepAabb: hit, entry parameter, entry-face normal -----------------
  {
    float tEnter = 0.0f;
    glm::vec2 normal{0.0f, 0.0f};
    // segment from (-2,0) moving +X by 4 into box [-1,-1]..[1,1]
    const bool hit = sweepAabb(
        {-2.0f, 0.0f}, {4.0f, 0.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, tEnter, normal);
    CHECK(hit);
    CHECK(testing::approx(tEnter, 0.25f)); // enters the low x face at t=0.25
    CHECK(testing::approx(normal.x, -1.0f));
    CHECK(testing::approx(normal.y, 0.0f));
  }

  // --- sweepAabb: parallel miss -------------------------------------------
  {
    float tEnter = 0.0f;
    glm::vec2 normal{0.0f, 0.0f};
    const bool hit = sweepAabb(
        {-2.0f, 5.0f}, {4.0f, 0.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, tEnter, normal);
    CHECK(!hit); // runs alongside the box, never crossing in
  }

  // --- sweepAabb: starting inside is not a crossing-in --------------------
  {
    float tEnter = 0.0f;
    glm::vec2 normal{0.0f, 0.0f};
    const bool hit = sweepAabb(
        {0.0f, 0.0f}, {4.0f, 0.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, tEnter, normal);
    CHECK(!hit);
  }

  // --- walkGridDDA: a horizontal ray visits the start tile then crossings --
  {
    std::vector<glm::ivec2> visited;
    walkGridDDA({0.5f, 0.5f},
                {1.0f, 0.0f},
                3.0f,
                [&](const glm::ivec2& tile, float)
                {
                  visited.push_back(tile);
                  return true;
                });
    CHECK(visited.size() == 4);
    CHECK(visited[0] == glm::ivec2(0, 0)); // start tile visited first
    CHECK(visited[1] == glm::ivec2(1, 0));
    CHECK(visited[2] == glm::ivec2(2, 0));
    CHECK(visited[3] == glm::ivec2(3, 0));
  }

  // --- walkGridDDA: returning false stops traversal early -----------------
  {
    int count = 0;
    walkGridDDA({0.5f, 0.5f},
                {1.0f, 0.0f},
                10.0f,
                [&](const glm::ivec2&, float)
                {
                  ++count;
                  return count < 2; // stop after the second tile
                });
    CHECK(count == 2);
  }

  // --- walkGridDDA: zero direction / distance does nothing harmful --------
  {
    int count = 0;
    walkGridDDA({0.0f, 0.0f},
                {0.0f, 0.0f},
                5.0f,
                [&](const glm::ivec2&, float)
                {
                  ++count;
                  return true;
                });
    CHECK(count == 0);
  }

  // --- clipQuadToRect: a large quad clips down to the unit rect -----------
  {
    const glm::vec2 pts[4] = {
        {-1.0f, -1.0f}, {2.0f, -1.0f}, {2.0f, 2.0f}, {-1.0f, 2.0f}};
    const glm::vec2 uvs[4] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    ClipVertex out[12];
    const int n = clipQuadToRect(pts, uvs, 0.0f, 0.0f, 1.0f, 1.0f, out);
    CHECK(n >= 4);
    for (int i = 0; i < n; ++i)
    {
      CHECK(out[i].p.x >= -1e-4f && out[i].p.x <= 1.0f + 1e-4f);
      CHECK(out[i].p.y >= -1e-4f && out[i].p.y <= 1.0f + 1e-4f);
      CHECK(out[i].uv.x >= -1e-4f && out[i].uv.x <= 1.0f + 1e-4f);
      CHECK(out[i].uv.y >= -1e-4f && out[i].uv.y <= 1.0f + 1e-4f);
    }
  }

  // --- clipQuadToRect: a quad fully outside the rect clips away -----------
  {
    const glm::vec2 pts[4] = {
        {5.0f, 5.0f}, {6.0f, 5.0f}, {6.0f, 6.0f}, {5.0f, 6.0f}};
    const glm::vec2 uvs[4] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    ClipVertex out[12];
    const int n = clipQuadToRect(pts, uvs, 0.0f, 0.0f, 1.0f, 1.0f, out);
    CHECK(n < 3); // nothing survives
  }

  return testing::report("algorithmsTests");
}
