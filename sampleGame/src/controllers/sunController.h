#pragma once

#include "config.h"
#include "engine/systems/isometric/isometricLightingSystem.h"
#include "glm/glm/geometric.hpp"
#include <algorithm>

class SunController
{
public:
  SunController() {}

  ~SunController() = default;

  void moveTo(int x, int y)
  {
    assert(m_lighting && "SunController renderer not set");

    if (!m_lighting)
      return;

    if (!m_isSunEnabled)
    {
      m_lighting->setAmbientDirection(glm::vec3{0.0f, 0.0f, 1.0f});
      m_lighting->setAmbient(0.06f);
      m_lighting->setAmbientDiffuseStrength(0.0f);
      m_lighting->setAmbientColor(glm::vec3{0.08f, 0.13f, 0.30f});
      return;
    }

    const float w = static_cast<float>(WINDOW_WIDTH);
    const float h = static_cast<float>(WINDOW_HEIGHT);
    const float cx = w * 0.5f;

    const float clampedY = std::clamp(static_cast<float>(y), 0.0f, h);
    const float clampedX = std::clamp(static_cast<float>(x), 0.0f, w);

    const float y01 = clampedY / h;

    //
    // Mouse top = noon
    // Mouse bottom = night
    //
    const float dayPosition = 1.0f - y01;

    //
    // Keep sun slightly above horizon at all times
    // so shadows remain possible.
    //
    constexpr float MinSunZ = 0.08f;

    float sunZ = MinSunZ + dayPosition * (1.0f - MinSunZ);

    //
    // Make shadows lengthen earlier while still bright.
    //
    const float shadowSunZ = std::pow(sunZ, 1.35f);

    const float horizontalAmount =
        std::sqrt(std::max(0.0f, 1.0f - shadowSunZ * shadowSunZ));

    const float side =
        std::clamp((static_cast<float>(clampedX) - cx) / cx, -1.0f, 1.0f);

    constexpr float BackLightBias = -0.65f;

    glm::vec2 horizontalDir{
        side,
        BackLightBias,
    };

    if (glm::length(horizontalDir) > 0.001f)
      horizontalDir = glm::normalize(horizontalDir);
    else
      horizontalDir = glm::vec2{0.0f, -1.0f};

    glm::vec3 sunDir{
        horizontalDir.x * horizontalAmount,
        horizontalDir.y * horizontalAmount,
        sunZ,
    };

    if (glm::length(sunDir) > 0.001f)
      sunDir = glm::normalize(sunDir);
    else
      sunDir = glm::vec3{0.0f, 0.0f, 1.0f};

    //
    // Exponential DAY -> NIGHT curve.
    //
    // Stays bright for most of the screen,
    // then falls off near the bottom.
    //
    const float ambientDaylight = 1.0f - std::pow(1.0f - dayPosition, 5.5f);

    const float diffuseDaylight = 1.0f - std::pow(1.0f - dayPosition, 3.2f);

    m_lighting->setAmbientDirection(sunDir);

    //
    // Brighter daytime while preserving dark nights.
    //
    m_lighting->setAmbient(0.06f + ambientDaylight * 0.39f);

    //
    // Directional sunlight strength.
    //
    m_lighting->setAmbientDiffuseStrength(diffuseDaylight * 0.78f);

    const glm::vec3 dayColor{
        1.0f,
        1.0f,
        1.0f,
    };

    const glm::vec3 nightColor{
        0.08f,
        0.13f,
        0.30f,
    };

    const glm::vec3 ambientColor =
        glm::mix(nightColor, dayColor, ambientDaylight);

    m_lighting->setAmbientColor(ambientColor);
  }

  void setLightingSystem(sfs::IsometricLightingSystem& lighting)
  {
    m_lighting = &lighting;
  }

  void toggleSun() { m_isSunEnabled = !m_isSunEnabled; }

private:
  sfs::IsometricLightingSystem* m_lighting = nullptr;

  bool m_isSunEnabled = true;
};
