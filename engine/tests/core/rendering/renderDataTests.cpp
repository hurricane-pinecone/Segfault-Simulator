#include "../../testHarness.h"

#include <engine/core/rendering/lighting.h>
#include <engine/core/sceneData/ambientLighting.h>
#include <engine/core/sceneData/celestialBodies.h>
#include <engine/core/sceneData/clouds.h>
#include <engine/core/sceneData/directionalLighting.h>
#include <engine/core/sceneData/shaderGlobals.h>
#include <engine/core/sceneData/wind.h>

#include "glm/glm/geometric.hpp"

#include <cmath>
#include <limits>
#include <string>

using namespace sfs;

int main()
{
  TEST("the wind's velocity should be its speed along its direction")
  {
    const glm::vec2 east = windVelocity(Wind{.direction = 0.0f, .speed = 4.0f});
    CHECK(glm::distance(east, glm::vec2(4.0f, 0.0f)) < 1e-5f);
    const glm::vec2 north =
        windVelocity(Wind{.direction = 90.0f, .speed = 2.0f});
    CHECK(glm::distance(north, glm::vec2(0.0f, 2.0f)) < 1e-5f);
    Wind broken;
    broken.speed = std::numeric_limits<float>::quiet_NaN();
    CHECK(windVelocity(broken) == glm::vec2(0.0f));
  }

  TEST("things should sway in proportion to the wind's speed, a breeze's at a "
       "breeze, between a light stir and a gale's")
  {
    CHECK(windSway(Wind{.speed = kBreezeSpeed}) == kSwayAtBreeze);
    CHECK(windSway(Wind{.speed = 2.0f * kBreezeSpeed}) == 2.0f * kSwayAtBreeze);
    CHECK(windSway(Wind{.speed = 0.0f}) == kLeastSway * kSwayAtBreeze);
    CHECK(windSway(Wind{.speed = 100.0f}) == kMostSway * kSwayAtBreeze);
    Wind broken;
    broken.speed = std::numeric_limits<float>::quiet_NaN();
    CHECK(windSway(broken) == kLeastSway * kSwayAtBreeze);
    // A new Wind blows a breeze.
    CHECK(windSway(Wind{}) == kSwayAtBreeze);
  }

  TEST("a scene's lighting should be its directional and ambient light, "
       "clouds and celestial bodies as they are, each optional")
  {
    DirectionalLighting light;
    light.direction = {0.0f, 1.0f, 1.0f};
    light.intensity = 0.4f;
    AmbientLighting ambient;
    ambient.intensity = 0.3f;
    ambient.skyColor = {0.1f, 0.2f, 0.3f};
    const Clouds clouds{.cover = 0.7f};
    const SceneLighting lit = sceneLighting(&light, &ambient, &clouds, nullptr);
    CHECK(lit.sun.direction == light.direction);
    CHECK(lit.sun.diffuseStrength == 0.4f);
    CHECK(lit.ambient == 0.3f);
    CHECK(lit.skyColor == ambient.skyColor);
    CHECK(lit.cloudCover == 0.7f);
    CHECK(lit.moon.direction.z < 0.0f); // no moon
    // None of them: no directional light, no ambient light, a clear sky.
    const SceneLighting none =
        sceneLighting(nullptr, nullptr, nullptr, nullptr);
    CHECK(none.sun.direction.z < 0.0f);
    CHECK(none.sun.diffuseStrength == 0.0f);
    CHECK(none.ambient == 0.0f);
    CHECK(none.skyColor == glm::vec3(0.0f));
    CHECK(none.cloudCover == 0.0f);
    // A physical sky's bodies: its sun and moon where they are.
    CelestialBodies bodies;
    bodies.sunDirection = {0.0f, -1.0f, -0.2f};
    bodies.moonDirection = {0.3f, 0.2f, 0.8f};
    bodies.moonPhase = 0.5f;
    const SceneLighting sky = sceneLighting(&light, &ambient, nullptr, &bodies);
    CHECK(sky.sun.direction == bodies.sunDirection);
    CHECK(sky.moon.direction == bodies.moonDirection);
    CHECK(sky.moon.phase == 0.5f);
    // The defaults are what a scene had before these were scene data.
    const DirectionalLighting newLight;
    const AmbientLighting newAmbient;
    const SceneLighting defaults =
        sceneLighting(&newLight, &newAmbient, nullptr, nullptr);
    const SceneLighting plain;
    CHECK(defaults.sun.direction == plain.sun.direction);
    CHECK(defaults.sun.diffuseStrength == plain.sun.diffuseStrength);
    CHECK(defaults.ambient == plain.ambient);
    CHECK(defaults.skyColor == plain.skyColor);
  }

  TEST("the moon's share of its light under clear skies should be a scene's "
       "under clouds, without them")
  {
    CelestialBodies bodies;
    bodies.sunDirection = {0.0f, -1.0f, -0.3f}; // set
    bodies.moonDirection = {0.2f, 0.1f, 0.9f};
    bodies.moonPhase = 0.8f;
    SceneLighting lighting;
    lighting.sun.direction = bodies.sunDirection;
    lighting.moon.direction = bodies.moonDirection;
    lighting.moon.phase = bodies.moonPhase;
    lighting.cloudCover = 0.5f;
    CHECK(moonlight(bodies) > 0.0f);
    CHECK(moonlight(lighting) == moonlight(bodies) * clearSky(0.5f));
  }

  TEST("global shader parameters should be set, read and gathered at their "
       "slots, the engine's own first")
  {
    CHECK(shaderGlobalSlot("wetness") ==
          static_cast<int>(kShaderGlobalWetness));
    CHECK(shaderGlobalSlot("puddles") ==
          static_cast<int>(kShaderGlobalPuddles));
    CHECK(shaderGlobalSlot("") == -1);
    const int tint = shaderGlobalSlot("tint");
    CHECK(tint > static_cast<int>(kShaderGlobalPuddles));
    CHECK(shaderGlobalSlot("tint") == tint);

    ShaderGlobals globals;
    CHECK(shaderGlobal(globals, "tint") == nullptr);
    setShaderGlobal(globals, "tint", glm::vec4(1.0f, 0.5f, 0.25f, 1.0f));
    setShaderGlobal(globals, "wetness", 0.5f);
    setShaderGlobal(globals, "wetness", 0.75f);
    CHECK(globals.parameters.size() == 2u);
    CHECK(shaderGlobal(globals, "wetness")->x == 0.75f);

    ShaderGlobalValues values;
    values.fill(glm::vec4(9.0f));
    gatherShaderGlobals(&globals, values);
    CHECK(values[kShaderGlobalWetness] == glm::vec4(0.75f, 0.0f, 0.0f, 0.0f));
    CHECK(values[static_cast<std::size_t>(tint)] ==
          glm::vec4(1.0f, 0.5f, 0.25f, 1.0f));
    CHECK(values[kShaderGlobalPuddles] == glm::vec4(0.0f));
    gatherShaderGlobals(nullptr, values);
    CHECK(values[static_cast<std::size_t>(tint)] == glm::vec4(0.0f));
  }

  TEST("gathering a scene's parameters should give none a slot: a name no "
       "shader reads (one half typed) is left out")
  {
    ShaderGlobals globals;
    for (const char* typing : {"g", "gl", "glo", "glow"})
      setShaderGlobal(globals, typing, 1.0f);
    ShaderGlobalValues values;
    gatherShaderGlobals(&globals, values);
    for (const char* typing : {"g", "gl", "glo", "glow"})
      CHECK(findShaderGlobalSlot(typing) == -1);
    // Once a shader reads it, it's gathered.
    const int glow = shaderGlobalSlot("glow");
    CHECK(findShaderGlobalSlot("glow") == glow);
    gatherShaderGlobals(&globals, values);
    CHECK(values[static_cast<std::size_t>(glow)].x == 1.0f);
  }

  TEST("once every slot is taken, another parameter should have none")
  {
    int last = 0;
    for (std::size_t i = 0; i < kMaxShaderGlobals; ++i)
      last = shaderGlobalSlot("filler" + std::to_string(i));
    CHECK(last == -1);
    CHECK(shaderGlobalSlot("wetness") == 0); // the engine's keep theirs
  }

  return testing::report("renderDataTests");
}
