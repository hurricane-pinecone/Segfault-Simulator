#pragma once

#include "engine/renderers/openGLQuadRenderer.h"

#include <memory>

namespace sfs
{

class RenderContext
{
public:
  static bool init(int windowWidth, int windowHeight);
  static void shutdown();

  static OpenGLQuadRenderer& quadRenderer();
  static bool isInitialized();

private:
  static bool m_initialized;
  static std::unique_ptr<OpenGLQuadRenderer> m_quadRenderer;
};

} // namespace sfs
