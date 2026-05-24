#pragma once

#include "SDL2/SDL_rect.h"
#include "SDL2/SDL_surface.h"
#include "engine/rendering/batchKeys/LitQuadBatchKey.h"
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

class OpenGLQuadRenderer
{
public:
  OpenGLQuadRenderer(int windowWidth, int windowHeight);
  ~OpenGLQuadRenderer();

  void initialize();
  void shutdown();

  unsigned int getOrCreateTexture(const std::string& textureId,
                                  SDL_Surface* surface);

  unsigned int uploadSurfaceTexture(SDL_Surface* surface);
  void deleteTexture(unsigned int texture);

  void submit(const Quad& command);
  void submit(const TexturedQuad& command);
  void submit(const FreeformQuad& command);
  void submit(const LitQuad& command);

  void drawImmediate(const TexturedQuad& command); // Text, UI, simple sprites

  void begin();
  void flush();

  void drawLineLoop(const glm::vec2* points, int count, SDL_Color color);

  void setViewportSize(int width, int height);

private:
  enum class Pipeline
  {
    None,
    SolidColor,
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
