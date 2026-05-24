#pragma once

#include "engine/rendering/openGLQuadRenderer.h"
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
