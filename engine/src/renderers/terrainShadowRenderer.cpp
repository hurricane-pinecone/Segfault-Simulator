#include "engine/renderers/terrainShadowRenderer.h"
#include "engine/renderers/openGLQuadRenderer.h"

#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif
#include <SDL_opengl.h>
#include <cstddef>
#include <string>

namespace sfs
{

TerrainShadowRenderer::TerrainShadowRenderer(int windowWidth, int windowHeight)
    : windowWidth(windowWidth), windowHeight(windowHeight)
{
}

TerrainShadowRenderer::~TerrainShadowRenderer() { shutdown(); }

void TerrainShadowRenderer::initialize()
{
  if (initialized)
    return;

  shaderProgram = createShaderProgram();

  if (shaderProgram == 0)
    return;

  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, position)));

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,
                        4,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, color)));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  initialized = true;
}

void TerrainShadowRenderer::shutdown()
{
  if (!initialized)
    return;

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  if (vbo != 0)
    glDeleteBuffers(1, &vbo);

  if (vao != 0)
    glDeleteVertexArrays(1, &vao);

  if (shaderProgram != 0)
    glDeleteProgram(shaderProgram);

  vbo = 0;
  vao = 0;
  shaderProgram = 0;

  vertices.clear();

  initialized = false;
}

void TerrainShadowRenderer::setViewportSize(int width, int height)
{
  windowWidth = width;
  windowHeight = height;
}

void TerrainShadowRenderer::begin() { vertices.clear(); }

void TerrainShadowRenderer::submitTriangle(glm::vec2 a,
                                           glm::vec2 b,
                                           glm::vec2 c,
                                           SDL_Color color)
{
  const glm::vec4 rgba{
      color.r / 255.0f,
      color.g / 255.0f,
      color.b / 255.0f,
      color.a / 255.0f,
  };

  vertices.push_back({toNdc(a), rgba});
  vertices.push_back({toNdc(b), rgba});
  vertices.push_back({toNdc(c), rgba});
}

void TerrainShadowRenderer::submitQuad(glm::vec2 a,
                                       glm::vec2 b,
                                       glm::vec2 c,
                                       glm::vec2 d,
                                       SDL_Color color)
{
  submitTriangle(a, b, c, color);
  submitTriangle(a, c, d, color);
}

void TerrainShadowRenderer::flush()
{
  initialize();

  if (!initialized)
    return;

  if (vertices.empty())
    return;

  glUseProgram(shaderProgram);

  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
               vertices.data(),
               GL_DYNAMIC_DRAW);

  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);

  vertices.clear();
}

glm::vec2 TerrainShadowRenderer::toNdc(glm::vec2 pixelPosition) const
{
  return {
      pixelPosition.x / static_cast<float>(windowWidth) * 2.0f - 1.0f,
      1.0f - pixelPosition.y / static_cast<float>(windowHeight) * 2.0f,
  };
}

unsigned int TerrainShadowRenderer::compileShader(unsigned int type,
                                                  const char* source) const
{
  GLuint shader = glCreateShader(type);

  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint success = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

  if (!success)
  {
    char infoLog[1024];
    glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);

    glDeleteShader(shader);
    return 0;
  }

  return shader;
}

unsigned int TerrainShadowRenderer::createShaderProgram() const
{
#ifdef __EMSCRIPTEN__
  const std::string glslVersion = "#version 300 es\n"
                                  "precision mediump float;\n";
#else
  const std::string glslVersion = "#version 330 core\n";
#endif

  const std::string vertexSource = glslVersion + R"(
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec4 aColor;

out vec4 vColor;

void main()
{
  vColor = aColor;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

  const std::string fragmentSource = glslVersion + R"(
in vec4 vColor;
out vec4 FragColor;

void main()
{
  FragColor = vColor;
}
)";

  GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource.c_str());
  GLuint fragmentShader =
      compileShader(GL_FRAGMENT_SHADER, fragmentSource.c_str());

  if (vertexShader == 0 || fragmentShader == 0)
  {
    if (vertexShader != 0)
      glDeleteShader(vertexShader);

    if (fragmentShader != 0)
      glDeleteShader(fragmentShader);

    return 0;
  }

  GLuint program = glCreateProgram();

  glAttachShader(program, vertexShader);
  glAttachShader(program, fragmentShader);
  glLinkProgram(program);

  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);

  GLint success = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &success);

  if (!success)
  {
    char infoLog[1024];
    glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);

    glDeleteProgram(program);
    return 0;
  }

  return program;
}

bool TerrainShadowRenderer::hasPending() const { return !vertices.empty(); }

} // namespace sfs
