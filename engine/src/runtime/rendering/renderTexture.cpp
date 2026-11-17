#include "engine/runtime/rendering/renderTexture.h"

#include "engine/runtime/rendering/uiDraw.h"

#include <glm/glm/common.hpp>
#include <glm/glm/ext/vector_int2.hpp>

namespace sfs
{

RenderTexture::RenderTexture(glm::ivec2 size)
{
  m_texture.id = nextUiTextureId();
  m_texture.size = glm::max(size, glm::ivec2(1));
  m_texture.gpu = true;
}

void RenderTexture::resize(glm::ivec2 size)
{
  m_texture.size = glm::max(size, glm::ivec2(1));
}

} // namespace sfs
