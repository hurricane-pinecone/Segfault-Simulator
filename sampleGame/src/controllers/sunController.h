#pragma once

#include "config.h"
#include "engine/systems/isometricLightingSystem.h"
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
      m_lighting->setLightDirection(glm::vec3{0.0f, 0.0f, 1.0f});
      m_lighting->setLighting(0.08f, 0.0f);
      return;
    }

    const float cx = static_cast<float>(WINDOW_WIDTH) * 0.5f;
    const float cy = static_cast<float>(WINDOW_HEIGHT) * 0.5f;

    float dx = (static_cast<float>(x) - cx) / cx;
    float dy = (cy - static_cast<float>(y)) / cy; // screen up = positive

    glm::vec2 mouseDir{dx, dy};

    float distanceFromCenter = std::clamp(glm::length(mouseDir), 0.0f, 1.0f);

    if (distanceFromCenter > 0.001f)
      mouseDir /= distanceFromCenter;
    else
      mouseDir = glm::vec2{0.0f, 1.0f};

    // Mouse angle around the screen = sun rotation around the world.
    float azimuth = std::atan2(mouseDir.y, mouseDir.x);

    // Center = high noon, edge = horizon.
    float y01 = std::clamp(y / static_cast<float>(WINDOW_HEIGHT), 0.0f, 1.0f);
    float sunZ = 1.0f - y01 * 2.0f;

    float horizontalRadius = std::sqrt(std::max(0.0f, 1.0f - sunZ * sunZ));

    glm::vec3 sunDir{
        std::cos(azimuth) * horizontalRadius,
        std::sin(azimuth) * horizontalRadius,
        sunZ,
    };

    if (glm::length(sunDir) > 0.001f)
      sunDir = glm::normalize(sunDir);

    float daylight = std::clamp((sunZ + 0.3f) / 1.2f, 0.0f, 1.0f);
    daylight = daylight * daylight * (3.0f - 2.0f * daylight);

    m_lighting->setLightDirection(sunDir);
    m_lighting->setLighting(0.08f + daylight * 0.28f, 0.12f + daylight * 0.78f);
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
