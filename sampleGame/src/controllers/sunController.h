#pragma once

#include "engine/core/ecs/system.h"
#include "engine/core/scripting/iLuaConfigurable.h"
#include "engine/core/scripting/luaScripting.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

class SunController : public sfs::System, public sfs::ILuaConfigurable
{
public:
  SunController() = default;

  // Drop our `sun` table from the VM before we're destroyed, so a torn-down
  // controller can't be reached from Lua. activeLua() is null during app
  // shutdown (the VM is gone first), making this a safe no-op then.
  ~SunController() override
  {
    if (sfs::LuaScripting* lua = sfs::activeLua())
      lua->unregisterConfig(*this);
  }

  void create() override
  {
    if (!registry)
      return;

    if (!registry->hasSystem<sfs::RenderSystem3D>())
      return;

    auto& renderSystem = registry->getSystem<sfs::RenderSystem3D>();

    m_renderSystem = &renderSystem;

    applyTimeOfDay(m_timeOfDay);
  }

  void update(double deltaTime) override
  {
    if (!m_renderSystem)
      return;

    if (!m_isSunEnabled)
    {
      applyNightLighting();
      return;
    }

    m_timeOfDay +=
        static_cast<float>(deltaTime) / m_dayLengthSeconds * m_timeMultiplier;

    m_timeOfDay -= glm::floor(m_timeOfDay);

    applyTimeOfDay(m_timeOfDay);
  }

  void toggleSun() { m_isSunEnabled = !m_isSunEnabled; }

  bool sunEnabled() const { return m_isSunEnabled; }
  void setSunEnabled(bool enabled) { m_isSunEnabled = enabled; }

  void setDayLengthSeconds(float seconds)
  {
    m_dayLengthSeconds = glm::max(seconds, 1.0f);
  }

  // 0 = paused, 1 = normal speed, >1 = faster day/night cycle.
  float timeMultiplier() const { return m_timeMultiplier; }
  void setTimeMultiplier(float multiplier)
  {
    m_timeMultiplier = glm::max(0.0f, multiplier);
  }

  float timeOfDay() const { return m_timeOfDay; }
  void setTimeOfDay(float time01)
  {
    m_timeOfDay = time01 - glm::floor(time01);

    if (m_renderSystem)
      applyTimeOfDay(m_timeOfDay);
  }

  std::string timeString12Hour() const
  {
    // 0.0 -> 24.0 hours
    const float totalHours = m_timeOfDay * 24.0f;

    int hour24 = static_cast<int>(glm::floor(totalHours)) % 24;

    const int minutes =
        static_cast<int>((totalHours - glm::floor(totalHours)) * 60.0f);

    const bool pm = hour24 >= 12;

    int hour12 = hour24 % 12;

    if (hour12 == 0)
      hour12 = 12;

    char buffer[32];

    std::snprintf(buffer,
                  sizeof(buffer),
                  "%d:%02d %s",
                  hour12,
                  minutes,
                  pm ? "PM" : "AM");

    return buffer;
  }

  std::string luaConfigName() const override { return "sun"; }

  sfs::LuaSchema luaConfigSchema() const override
  {
    return {
        sfs::field("enabled",
                   &SunController::m_isSunEnabled,
                   "bool: run the day/night cycle"),
        sfs::field("dayLengthSeconds",
                   &SunController::m_dayLengthSeconds,
                   "number: real seconds per full day"),
        sfs::field("timeMultiplier",
                   &SunController::m_timeMultiplier,
                   "number: cycle speed (0 = paused)"),
        sfs::field("timeOfDay",
                   &SunController::m_timeOfDay,
                   "0..1: .0 midnight .25 sunrise .5 noon .75 sunset"),
    };
  }

  void* luaConfigData() override { return this; }

  // set{} writes the raw fields, bypassing the setters' clamps -- re-apply them
  // and refresh the lighting so a live edit takes effect immediately.
  void onLuaConfigChanged() override
  {
    m_dayLengthSeconds = glm::max(m_dayLengthSeconds, 1.0f);
    m_timeMultiplier = glm::max(0.0f, m_timeMultiplier);
    m_timeOfDay -= glm::floor(m_timeOfDay);
    if (m_renderSystem)
      applyTimeOfDay(m_timeOfDay);
  }

private:
  void applyNightLighting()
  {
    apply(glm::vec3{0.0f, 0.0f, 1.0f}, 0.06f, 0.0f, {0.08f, 0.13f, 0.30f});
  }

  void applyTimeOfDay(float t)
  {
    constexpr float TwoPi = 6.28318530718f;

    auto saturate = [](float x) { return glm::clamp(x, 0.0f, 1.0f); };

    auto ramp = [&](float start, float end, float x)
    { return saturate((x - start) / (end - start)); };

    // 6 AM -> 6 PM sun travel.
    const float dayProgress = ramp(6.0f / 24.0f, 18.0f / 24.0f, t);

    // Sideways sun motion: morning east, noon center, evening west.
    const float sunX = glm::cos(dayProgress * 3.14159265359f);

    // Very high at midday, lower near sunrise/sunset.
    const float noonAmount =
        saturate(1.0f - glm::abs(dayProgress * 2.0f - 1.0f));

    const float smoothNoon =
        noonAmount * noonAmount * (3.0f - 2.0f * noonAmount);

    // Low at sunrise/sunset, almost overhead at noon.
    const float sunZ = 0.08f + smoothNoon * 0.91f;

    // Ambient is basically full daylight from 7 AM to 5 PM.
    const float morning = ramp(5.0f / 24.0f, 7.0f / 24.0f, t);
    const float evening = 1.0f - ramp(17.0f / 24.0f, 19.0f / 24.0f, t);

    const float daylight = saturate(morning * evening);

    constexpr float BackLightBias = -0.65f;

    glm::vec2 horizontalDir{sunX, BackLightBias};

    if (glm::length(horizontalDir) > 0.001f)
      horizontalDir = glm::normalize(horizontalDir);
    else
      horizontalDir = glm::vec2{0.0f, -1.0f};

    const float horizontalAmount =
        glm::sqrt(glm::max(0.0f, 1.0f - sunZ * sunZ));

    glm::vec3 sunDir{
        horizontalDir.x * horizontalAmount,
        horizontalDir.y * horizontalAmount,
        sunZ,
    };

    sunDir = glm::normalize(sunDir);

    const glm::vec3 nightColor{0.03f, 0.05f, 0.13f};
    const glm::vec3 dayColor{1.0f, 1.0f, 1.0f};
    const glm::vec3 duskColor{1.0f, 0.62f, 0.35f};

    const float dusk = ramp(16.5f / 24.0f, 18.5f / 24.0f, t);

    const glm::vec3 daylightColor = glm::mix(dayColor, duskColor, dusk);

    // Shading: strongest when the sun is low, softer at noon.
    apply(sunDir,
          0.02f + daylight * 0.98f,
          daylight * glm::mix(0.65f, 0.30f, noonAmount),
          glm::mix(nightColor, daylightColor, daylight));
  }

  // The scene's light: the sun from `direction` on the grid (z up), the
  // ambient level, how much the sun's direction shades, and the light's
  // colour (the sky a dim shade of it).
  void apply(const glm::vec3& direction,
             float ambient,
             float diffuseStrength,
             const glm::vec3& color)
  {
    sfs::SceneLighting3D lighting;
    const auto& space = m_renderSystem->space2D();
    lighting.sun.direction =
        space ? glm::normalize(space->direction(direction)) : direction;
    lighting.sun.diffuseStrength = diffuseStrength;
    lighting.sun.color = color;
    lighting.ambient = ambient;
    lighting.skyColor = color * (0.05f + 0.25f * ambient);
    m_renderSystem->setLighting(lighting);
  }

private:
  sfs::RenderSystem3D* m_renderSystem = nullptr;

  bool m_isSunEnabled = true;

  // 0.00 = midnight
  // 0.25 = sunrise
  // 0.50 = noon
  // 0.75 = sunset
  float m_timeOfDay = 0.0f;

  // Full day duration in real seconds.
  float m_dayLengthSeconds = 120.0f;

  // Scales how fast the cycle advances (debug control).
  float m_timeMultiplier = 1.0f;
};
