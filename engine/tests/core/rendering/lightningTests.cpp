#include "../../testHarness.h"

#include <engine/core/rendering/lightning.h>

#include <cmath>

using namespace sfs;

int main()
{
  TEST("a strike should flash at once, flicker with its strokes, then go "
       "dark")
  {
    for (uint32_t seed = 0; seed < 50; ++seed)
    {
      for (const bool inCloud : {false, true})
      {
        LightningStrike strike;
        strike.seed = seed;
        strike.inCloud = inCloud;
        CHECK(std::abs(lightningBrightness(strike, 0.0f) - 1.0f) < 1e-4f);
        const float duration = lightningDuration(strike);
        CHECK(duration > 0.2f);
        CHECK(duration < 1.8f);
        CHECK(lightningBrightness(strike, duration) < 0.01f);
        CHECK(lightningChannelBrightness(strike, duration + 0.01f) < 0.011f);
        CHECK(lightningBrightness(strike, -0.1f) == 0.0f);

        // Flickers: it dims and brightens again (a later stroke) at least
        // once.
        int rises = 0;
        float last = 1.0f;
        bool falling = true;
        for (float t = 0.0f; t < duration; t += 0.002f)
        {
          const float light = lightningBrightness(strike, t);
          if (falling && light > last + 0.05f)
          {
            ++rises;
            falling = false;
          }
          else if (light < last)
            falling = true;
          last = light;
        }
        CHECK(rises >= 1 || inCloud);

        // The channel's afterimage outlasts its light.
        const float after = duration * 0.9f;
        CHECK(lightningChannelBrightness(strike, after) >=
              lightningBrightness(strike, after));
      }
    }

    // The same seed, the same strike; another, another.
    LightningStrike a;
    a.seed = 3;
    LightningStrike b = a;
    CHECK(lightningDuration(a) == lightningDuration(b));
    b.seed = 4;
    bool differs = false;
    for (float t = 0.0f; t < 0.6f; t += 0.01f)
      differs =
          differs || lightningBrightness(a, t) != lightningBrightness(b, t);
    CHECK(differs);
  }

  TEST("a strike's channel should run jagged from the cloud to the ground, "
       "with branches that don't reach it")
  {
    LightningStrike strike;
    strike.position = {100.0f, -50.0f, 4.0f};
    strike.seed = 11;
    const glm::vec3 top = lightningTop(strike, 1500.0f);
    CHECK(std::abs(top.z - 1500.0f) < 1e-3f);
    CHECK(glm::length(glm::vec2(top) - glm::vec2(strike.position)) < 450.0f);

    const std::vector<LightningSegment> channel = lightningChannel(strike, top);
    std::size_t main = 0;
    std::size_t branches = 0;
    float pathLength = 0.0f;
    for (const LightningSegment& segment : channel)
    {
      if (segment.branch)
      {
        ++branches;
        CHECK(segment.from.z > strike.position.z + 1.0f);
        continue;
      }
      ++main;
      pathLength += glm::distance(segment.from, segment.to);
    }
    CHECK(main == 128);
    CHECK(branches > 0);
    // It starts at the top and ends on the ground, and is longer than a
    // straight line (jagged).
    CHECK(glm::distance(channel.front().from, top) < 1e-3f);
    CHECK(glm::distance(channel[main - 1].to, strike.position) < 1e-3f);
    CHECK(pathLength > glm::distance(top, strike.position) * 1.1f);

    // The same every time; an in-cloud strike has none.
    CHECK(lightningChannel(strike, top).size() == channel.size());
    strike.inCloud = true;
    CHECK(lightningChannel(strike, top).empty());
  }

  TEST("a channel's mesh should be ribbons facing the eye, bright down the "
       "middle")
  {
    std::vector<LightningSegment> segments{
        {{0, 0, 100}, {0, 0, 0}, 1.0f, 1.0f, false},
        {{0, 0, 100}, {10, 0, 50}, 0.5f, 0.4f, true}};
    const MeshData main = lightningMesh(segments, {50, 0, 50}, 2.0f, false);
    CHECK(main.vertices.size() == 6);
    CHECK(main.indices.size() == 12);
    CHECK(main.sections.size() == 1);
    CHECK(main.sections[0].material.unlit);
    CHECK(main.sections[0].material.alphaMode == AlphaMode::Additive);
    // Across the view (y), 2 m wide; the middle lit, the edges not.
    CHECK(std::abs(std::abs(main.vertices[0].position.y) - 1.0f) < 1e-4f);
    CHECK(main.vertices[1].color.r == 1.0f);
    CHECK(main.vertices[0].color.r == 0.0f);
    // Its triangles face the eye (counter-clockwise seen from it).
    for (std::size_t i = 0; i < main.indices.size(); i += 3)
    {
      const glm::vec3 a = main.vertices[main.indices[i]].position;
      const glm::vec3 b = main.vertices[main.indices[i + 1]].position;
      const glm::vec3 c = main.vertices[main.indices[i + 2]].position;
      CHECK(glm::dot(glm::cross(b - a, c - a), glm::vec3(50, 0, 50) - a) >
            0.0f);
    }
    const MeshData branch = lightningMesh(segments, {50, 0, 50}, 2.0f, true);
    CHECK(branch.vertices.size() == 6);
    CHECK(lightningMesh({}, {0, 0, 0}, 1.0f, false).empty());
  }

  TEST("a channel should light as a line: 1 / the distance near it, 1 / its "
       "square far off, from along it")
  {
    const glm::vec3 low(0.0f);
    const glm::vec3 high(0.0f, 0.0f, 1000.0f);
    // Beside its middle, close: about pi / h.
    const float near = lightningLineLight(low, high, {10.0f, 0.0f, 500.0f});
    CHECK(std::abs(near - 3.14159f / 10.0f) < 0.01f);
    // Far off: about its length / d^2.
    const float far = lightningLineLight(low, high, {100000.0f, 0.0f, 0.0f});
    CHECK(std::abs(far / (1000.0f / 1e10f) - 1.0f) < 0.01f);
    // Its light comes from beside a point close by, and from higher up the
    // channel for one on the ground further off.
    CHECK(std::abs(lightningLightCentre(low, high, {10, 0, 500}).z - 500.0f) <
          1.0f);
    const glm::vec3 centre = lightningLightCentre(low, high, {500, 0, 0});
    CHECK(centre.z > 100.0f);
    CHECK(centre.z < 600.0f);
  }

  return testing::report("lightningTests");
}
