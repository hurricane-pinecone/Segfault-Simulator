#pragma once

#include "config.h"
#include "engine/systems/isometric/isometricLightingSystem.h"
#include "glm/glm/geometric.hpp"
#include <algorithm>

class SunController
{
public:
  SunController() = default;
  ~SunController() = default;

  void moveTo(int x, int y)
  {
    assert(m_lighting && "SunController lighting service not set");

    if (!m_lighting)
      return;

    if (!m_isSunEnabled)
    {
      sfs::IsometricAmbientLighting ambient;
      ambient.direction = glm::vec3{0.0f, 0.0f, 1.0f};
      ambient.ambient = 0.06f;
      ambient.diffuseStrength = 0.0f;
      ambient.color = glm::vec3{0.08f, 0.13f, 0.30f};

      m_lighting->setAmbientLighting(ambient);
      return;
    }

    const float w = static_cast<float>(WINDOW_WIDTH);
    const float h = static_cast<float>(WINDOW_HEIGHT);
    const float cx = w * 0.5f;

    const float clampedY = std::clamp(static_cast<float>(y), 0.0f, h);
    const float clampedX = std::clamp(static_cast<float>(x), 0.0f, w);

    const float y01 = clampedY / h;
    const float dayPosition = 1.0f - y01;

    constexpr float MinSunZ = 0.08f;

    float sunZ = MinSunZ + dayPosition * (1.0f - MinSunZ);
    const float shadowSunZ = std::pow(sunZ, 1.35f);

    const float horizontalAmount =
        std::sqrt(std::max(0.0f, 1.0f - shadowSunZ * shadowSunZ));

    const float side = std::clamp((clampedX - cx) / cx, -1.0f, 1.0f);

    constexpr float BackLightBias = -0.65f;

    glm::vec2 horizontalDir{side, BackLightBias};

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

    const float ambientDaylight = 1.0f - std::pow(1.0f - dayPosition, 5.5f);

    const float diffuseDaylight = 1.0f - std::pow(1.0f - dayPosition, 3.2f);

    const glm::vec3 dayColor{1.0f, 1.0f, 1.0f};
    const glm::vec3 nightColor{0.08f, 0.13f, 0.30f};

    sfs::IsometricAmbientLighting ambient;
    ambient.direction = sunDir;
    ambient.ambient = 0.06f + ambientDaylight * 0.39f;
    ambient.diffuseStrength = diffuseDaylight * 0.78f;
    ambient.color = glm::mix(nightColor, dayColor, ambientDaylight);

    m_lighting->setAmbientLighting(ambient);
  }

  void setLightingService(sfs::IsometricLightingService& lighting)
  {
    m_lighting = &lighting;
  }

  void toggleSun() { m_isSunEnabled = !m_isSunEnabled; }

private:
  sfs::IsometricLightingService* m_lighting = nullptr;
  bool m_isSunEnabled = true;
};
