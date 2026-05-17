#pragma once

#include "engine/renderers/openGLQuadRenderer.h"
#include "engine/renderers/terrainShadowRenderer.h"

#include <memory>

namespace sfs
{

class RenderContext
{
public:
  static bool init(int windowWidth, int windowHeight);
  static void shutdown();

  static OpenGLQuadRenderer& quadRenderer();
  static TerrainShadowRenderer& terrainShadowRenderer();
  static bool isInitialized();

private:
  static bool m_initialized;
  static std::unique_ptr<OpenGLQuadRenderer> m_quadRenderer;
  static std::unique_ptr<TerrainShadowRenderer> m_terrainShadowRenderer;
};

} // namespace sfs
