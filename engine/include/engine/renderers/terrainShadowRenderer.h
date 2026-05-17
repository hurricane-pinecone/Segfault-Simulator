#pragma once

#include "SDL2/SDL_pixels.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"

namespace sfs
{

class TerrainShadowRenderer
{
public:
  struct Vertex
  {
    glm::vec2 position;
    glm::vec4 color;
  };

  TerrainShadowRenderer(int windowWidth, int windowHeight);
  ~TerrainShadowRenderer();

  void initialize();
  void shutdown();

  void setViewportSize(int width, int height);

  void begin();
  void submitTriangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, SDL_Color color);

  void submitQuad(glm::vec2 a,
                  glm::vec2 b,
                  glm::vec2 c,
                  glm::vec2 d,
                  SDL_Color color);

  void flush();

  bool hasPending() const;

private:
  glm::vec2 toNdc(glm::vec2 pixelPosition) const;

  unsigned int compileShader(unsigned int type, const char* source) const;
  unsigned int createShaderProgram() const;

private:
  int windowWidth = 0;
  int windowHeight = 0;

  bool initialized = false;

  unsigned int shaderProgram = 0;
  unsigned int vao = 0;
  unsigned int vbo = 0;

  std::vector<Vertex> vertices;
};

} // namespace sfs
