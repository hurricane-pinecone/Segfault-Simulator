#pragma once

#include "SDL2/SDL_rect.h"
#include "SDL2/SDL_surface.h"
#include "engine/rendering/batchKeys/LitQuadBatchKey.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/iQuadRenderer.h"
#include "engine/rendering/quads.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <string>
#include <unordered_map>
#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

namespace sfs
{

class OpenGLQuadRenderer : public IQuadRenderer
{
public:
  OpenGLQuadRenderer(int windowWidth, int windowHeight);
  ~OpenGLQuadRenderer() override;

  void initialize() override;
  void shutdown() override;

  unsigned int getOrCreateTexture(const std::string& textureId,
                                  SDL_Surface* surface) override;

  unsigned int uploadSurfaceTexture(SDL_Surface* surface) override;
  void deleteTexture(unsigned int texture) override;

  void submit(const Quad& command) override;
  void submit(const TexturedQuad& command) override;
  void submit(const FreeformQuad& command) override;
  void submit(const LitQuad& command) override;
  void submit(const SurfaceCommand& command) override;
  void submitTerrainShadow(const Quad& command) override
  {
    initialize();

    if (!initialized)
      return;

    beginPipeline(Pipeline::TerrainShadow);
    appendSolidVertices(command);
  }

  void
  drawImmediate(const TexturedQuad& command) override; // Text, UI, sprites

  void begin() override;
  void flush() override;

  void
  drawLineLoop(const glm::vec2* points, int count, SDL_Color color) override;

  void setViewportSize(int width, int height) override;

  void setSurfaceTime(float time) override;

private:
  enum class Pipeline
  {
    None,
    SolidColor,
    TerrainShadow,
    Textured,
    Freeform,
    LitSprite
  };

  unsigned int createSolidShaderProgram() const;

  void beginPipeline(Pipeline stage);
  void flushCurrentPipeline();
  void flushSolid();
  void flushFreeform();
  void flushLit();

  void appendSolidVertices(const Quad& command);
  void appendLitVertices(const LitQuad& command);

  void drawQuad(const Quad& command);
  void drawQuad(const FreeformQuad& command); // Shadows
  void drawQuad(const LitQuad& command);      // Normalised sprites (ie,
                                              // sprites that light affects)

  glm::vec2 toNdc(const glm::vec2& pixelPosition) const;

  void drawSolidQuad(const Quad& command);
  void drawQuadInternal(unsigned int texture,
                        const SDL_Rect& srcRect,
                        int textureWidth,
                        int textureHeight,
                        const glm::vec2& p0,
                        const glm::vec2& p1,
                        const glm::vec2& p2,
                        const glm::vec2& p3,
                        SDL_Color tint);

  void drawQuadInternal(unsigned int texture,
                        const SDL_Rect& srcRect,
                        int textureWidth,
                        int textureHeight,
                        const glm::vec2& p0,
                        const glm::vec2& p1,
                        const glm::vec2& p2,
                        const glm::vec2& p3,
                        SDL_Color tint,
                        bool useLighting,
                        bool hasNormalMap,
                        unsigned int normalTexture,
                        const glm::vec3& lightDirection,
                        float lightIntensity,
                        float ambient,
                        float diffuseStrength,
                        const glm::vec3& lightColor,
                        const glm::vec2 worldPoints[4],
                        int lightCount,
                        const glm::vec2 lightPositions[MaxShaderLights],
                        const glm::vec3 lightColors[MaxShaderLights],
                        const float lightIntensities[MaxShaderLights],
                        const float lightRadii[MaxShaderLights],
                        const float lightHeights[MaxShaderLights]);
  void drawQuadInternalWithUvs(unsigned int texture,
                               const glm::vec2& p0,
                               const glm::vec2& p1,
                               const glm::vec2& p2,
                               const glm::vec2& p3,
                               const glm::vec2 uvs[4],
                               SDL_Color tint);

  unsigned int compileShader(unsigned int type, const char* source) const;
  unsigned int createShaderProgram() const;
  unsigned int createSurfaceShaderProgram() const;

  void flushTerrainShadow()
  {
    initialize();

    if (!initialized || m_solidVertices.empty())
      return;

    glUseProgram(solidShaderProgram);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_STENCIL_TEST);
    glClear(GL_STENCIL_BUFFER_BIT);

    glStencilMask(0xFF);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);

    glBindVertexArray(solidVao);
    glBindBuffer(GL_ARRAY_BUFFER, solidVbo);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(m_solidVertices.size() * sizeof(SolidVertex)),
        m_solidVertices.data(),
        GL_DYNAMIC_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_solidVertices.size()));

    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);

    m_solidVertices.clear();
  }

private:
  struct Vertex
  {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec2 worldPosition;
  };

  struct SolidVertex
  {
    glm::vec2 position;
    glm::vec4 color;
  };

  struct SurfaceGpuVertex
  {
    glm::vec2 position;
    glm::vec2 worldPosition;
    glm::vec4 color;
    glm::vec2 uv;
    glm::vec4 params;
  };

private:
  int windowWidth = 0;
  int windowHeight = 0;

  bool initialized = false;

  unsigned int shaderProgram = 0;
  unsigned int vao = 0;
  unsigned int vbo = 0;

  unsigned int defaultNormalTexture = 0;
  unsigned int whiteTexture = 0;

  int uUseLightingLocation = -1;
  int uNormalTextureLocation = -1;
  int uHasNormalMapLocation = -1;
  int uLightDirectionLocation = -1;
  int uLightIntensityLocation = -1;
  int uAmbientLocation = -1;
  int uDiffuseStrengthLocation = -1;
  int uTextureLocation = -1;
  int uColorLocation = -1;
  int uLightColorLocation = -1;
  GLint uLightCountLocation = -1;
  GLint uLightPositionsLocation = -1;
  GLint uLightColorsLocation = -1;
  GLint uLightIntensitiesLocation = -1;
  GLint uLightRadiiLocation = -1;
  GLint uLightHeightsLocation = -1;
  int uSurfaceEffectLocation = -1;
  int uSurfaceEffectTimeLocation = -1;

  unsigned int surfaceShaderProgram = 0;
  unsigned int surfaceVao = 0;
  unsigned int surfaceVbo = 0;
  unsigned int surfaceEbo = 0;

  int uSurfaceTimeLocation = -1;
  int uSurfaceRippleStrengthLocation = -1;
  int uSurfaceRippleScaleLocation = -1;
  int uSurfaceAmbientLocation = -1;
  int uSurfaceLightCountLocation = -1;
  int uSurfaceLightPositionsLocation = -1;
  int uSurfaceLightColorsLocation = -1;
  int uSurfaceLightIntensitiesLocation = -1;
  int uSurfaceLightRadiiLocation = -1;

  float m_surfaceTime = 0.0f;

  std::unordered_map<std::string, unsigned int> textureCache;

  // Batched

  Pipeline m_pipeline = Pipeline::None;

  unsigned int solidShaderProgram = 0;
  unsigned int solidVao = 0;
  unsigned int solidVbo = 0;

  std::vector<SolidVertex> m_solidVertices;
  std::vector<Vertex> m_litVertices;
  std::optional<LitBatchKey> m_litBatchKey;
};

} // namespace sfs
