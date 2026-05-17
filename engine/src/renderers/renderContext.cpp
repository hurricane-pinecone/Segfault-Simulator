#include "engine/renderers/renderContext.h"
#include "engine/renderers/terrainShadowRenderer.h"

namespace sfs
{

bool RenderContext::m_initialized = false;
std::unique_ptr<OpenGLQuadRenderer> RenderContext::m_quadRenderer = nullptr;
std::unique_ptr<TerrainShadowRenderer> RenderContext::m_terrainShadowRenderer =
    nullptr;

bool RenderContext::init(int windowWidth, int windowHeight)
{
  m_quadRenderer =
      std::make_unique<OpenGLQuadRenderer>(windowWidth, windowHeight);

  m_quadRenderer->initialize();
  m_quadRenderer->setViewportSize(windowWidth, windowHeight);

  m_terrainShadowRenderer =
      std::make_unique<TerrainShadowRenderer>(windowWidth, windowHeight);

  m_terrainShadowRenderer->initialize();
  m_terrainShadowRenderer->setViewportSize(windowWidth, windowHeight);

  m_initialized = true;
  return true;
}

void RenderContext::shutdown()
{
  if (m_terrainShadowRenderer)
    m_terrainShadowRenderer->shutdown();

  if (m_quadRenderer)
    m_quadRenderer->shutdown();

  m_terrainShadowRenderer.reset();
  m_quadRenderer.reset();

  m_initialized = false;
}

OpenGLQuadRenderer& RenderContext::quadRenderer()
{
  assert(m_quadRenderer && "RenderContext::quadRenderer() called before init");
  return *m_quadRenderer;
}

bool RenderContext::isInitialized()
{
  return m_initialized && m_quadRenderer != nullptr &&
         m_terrainShadowRenderer != nullptr;
}

TerrainShadowRenderer& RenderContext::terrainShadowRenderer()
{
  assert(m_terrainShadowRenderer &&
         "RenderContext::terrainShadowRenderer() called before init");
  return *m_terrainShadowRenderer;
}

} // namespace sfs
