#pragma once

#include "engine/systems/isometricRenderSystem.h"
#include <algorithm>

class SunController
{
public:
  SunController() {}

  ~SunController() = default;

  void moveTo(int x, int y)
  {
    assert(m_renderer && "SunController renderer not set");
    if (!m_renderer)
      return;

    if (!m_isSunEnabled)
    {
      m_renderer->setLightDirection(glm::vec3{0.0f, 0.0f, 1.0f});

      // ambient-only night lighting
      m_renderer->setLighting(0.08f, 0.0f);
      return;
    }

    float x01 = std::clamp(x / 800.0f, 0.0f, 1.0f);
    float y01 = std::clamp(y / 600.0f, 0.0f, 1.0f);

    float sunX = x01 * 2.0f - 1.0f;
    float sunHeight = 1.0f - y01;

    // Below this, sun is basically below horizon.
    float daylight = std::clamp((sunHeight - 0.15f) / 0.85f, 0.0f, 1.0f);
    daylight = daylight * daylight * (3.0f - 2.0f * daylight);

    // Allows bottom screen to behave like below-horizon light.
    float sunZ = sunHeight * 2.0f - 0.35f;

    m_renderer->setLightDirection(glm::vec3{sunX, 0.0f, sunZ});
    m_renderer->setLighting(0.08f + daylight * 0.28f, 0.12f + daylight * 0.78f);
  }

  void setRenderSystem(sfs::IsometricRenderSystem& renderer)
  {
    m_renderer = &renderer;
  }

  void toggleSun() { m_isSunEnabled = !m_isSunEnabled; }

private:
  sfs::IsometricRenderSystem* m_renderer = nullptr;

  bool m_isSunEnabled = true;
};
