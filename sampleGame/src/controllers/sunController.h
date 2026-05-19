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
      m_lighting->setAmbient(0.08f);
      m_lighting->setAmbientDiffuseStrength(0.0f);
      return;
    }

    const float w = static_cast<float>(WINDOW_WIDTH);
    const float h = static_cast<float>(WINDOW_HEIGHT);

    const float cx = w * 0.5f;

    const float x01 = std::clamp(static_cast<float>(x) / w, 0.0f, 1.0f);
    const float y01 = std::clamp(static_cast<float>(y) / h, 0.0f, 1.0f);

    // Vertical mouse controls sun height.
    // Top = high sun.
    // Bottom = low sun.
    float sunZ = 1.0f - y01;

    // Keep sun above horizon.
    sunZ = std::clamp(sunZ, 0.18f, 1.0f);

    // Center/top = near-noon.
    // Low sun = stronger sideways component.
    const float horizontalAmount =
        std::sqrt(std::max(0.0f, 1.0f - sunZ * sunZ));

    // Mouse X moves sun from left side to right side.
    // This controls whether shadows go front-right or front-left.
    const float side =
        std::clamp((static_cast<float>(x) - cx) / cx, -1.0f, 1.0f);

    // IMPORTANT:
    // lightDir is the direction light travels FROM.
    // shadows use -lightDir.
    //
    // We want shadows to always cast toward camera/front:
    //   -lightDir.y should be positive
    //
    // Therefore lightDir.y should stay negative.
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

    float daylight = std::clamp((sunZ - 0.12f) / 0.88f, 0.0f, 1.0f);
    daylight = daylight * daylight * (3.0f - 2.0f * daylight);

    m_lighting->setAmbientDirection(sunDir);
    m_lighting->setAmbient(0.08f + daylight * 0.28f);
    m_lighting->setAmbientDiffuseStrength(0.12f + daylight * 0.78f);
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
