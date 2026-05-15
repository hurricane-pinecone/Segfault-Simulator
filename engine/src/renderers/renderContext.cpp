#include "engine/renderers/renderContext.h"

namespace sfs
{

bool RenderContext::m_initialized = false;
std::unique_ptr<OpenGLQuadRenderer> RenderContext::m_quadRenderer = nullptr;

bool RenderContext::init(int windowWidth, int windowHeight)
{
  m_quadRenderer =
      std::make_unique<OpenGLQuadRenderer>(windowWidth, windowHeight);

  m_quadRenderer->initialize();
  m_quadRenderer->setViewportSize(windowWidth, windowHeight);

  m_initialized = true;
  return true;
}

void RenderContext::shutdown()
{
  if (m_quadRenderer)
    m_quadRenderer->shutdown();

  m_quadRenderer.reset();
  m_initialized = false;
}

OpenGLQuadRenderer& RenderContext::quadRenderer() { return *m_quadRenderer; }

bool RenderContext::isInitialized()
{
  return m_initialized && m_quadRenderer != nullptr;
}

} // namespace sfs
