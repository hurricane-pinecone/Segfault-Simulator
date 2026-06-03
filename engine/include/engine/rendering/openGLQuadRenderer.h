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
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
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
  void submitLitBatch(const LitQuadBatch& batch,
                      unsigned int texture,
                      unsigned int normalTexture,
                      bool hasNormalMap,
                      int surfaceEffect) override;
  void submit(const SurfaceCommand& command) override;
  void submitTerrainShadow(const Quad& command) override
  {
    initialize();

    if (!initialized)
      return;

    beginPipeline(Pipeline::TerrainShadow);
    appendSolidVertices(command);
  }

  void submitSpriteShadow(const FreeformQuad& command) override;

  void submitParticleBatch(const ParticleBatch& batch,
                           unsigned int texture,
                           BlendMode blend,
                           bool depthTested) override;

  void
  drawImmediate(const TexturedQuad& command) override; // Text, UI, sprites

  void begin() override;
  void flush() override;

  void
  drawLineLoop(const glm::vec2* points, int count, SDL_Color color) override;

  void setViewportSize(int width, int height) override;

  void setSurfaceTime(float time) override;

  void setPointLights(const PointLightSet& lights) override;

  void setHeightmap(const int* elevations,
                    int width,
                    int height,
                    int originX,
                    int originY,
                    float heightScale) override;

private:
  enum class Pipeline
  {
    None,
    SolidColor,
    TerrainShadow,
    SpriteShadow,
    Textured,
    Freeform,
    LitSprite,
    Particle
  };

  unsigned int createSolidShaderProgram() const;
  unsigned int createSpriteShadowShaderProgram() const;
  unsigned int createParticleShaderProgram() const;

  void beginPipeline(Pipeline stage);
  void flushCurrentPipeline();
  void flushSolid();
  void flushFreeform();
  void flushLit();
  void flushSpriteShadow();
  void flushParticles();

  // Binds the terrain heightmap to texture unit 2 and pushes its uniforms on the
  // currently-bound program. Every path that does point-light occlusion must call
  // this; otherwise the shader samples the heightmap from whatever state a prior
  // draw happened to leave behind.
  void bindHeightmapUniforms();

  void appendSolidVertices(const Quad& command);
  void appendLitVertices(const LitQuad& command);
  void appendSpriteShadowVertices(const FreeformQuad& command);
  void appendParticleVertices(const ParticleBatch& batch,
                              unsigned int texture,
                              BlendMode blend,
                              bool depthTested);

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
                               SDL_Color tint,
                               float z);

  unsigned int compileShader(unsigned int type, const char* source) const;
  unsigned int createShaderProgram() const;
  unsigned int createSurfaceShaderProgram() const;

  void flushTerrainShadow()
  {
    initialize();

    if (!initialized || m_solidVertices.empty())
      return;

    glUseProgram(solidShaderProgram);

    // Terrain shadows are translucent: test against the opaque depth (a block
    // in front occludes the shadow) but do not write depth. Stencil de-dups
    // overlapping shadow quads so they don't stack darker.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
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
    float z = 0.0f; // clip-space depth (gl_Position.z)
  };

  struct SolidVertex
  {
    glm::vec2 position;
    glm::vec4 color;
    float z = 0.0f; // clip-space depth (gl_Position.z)
  };

  struct SurfaceGpuVertex
  {
    glm::vec2 position;
    glm::vec2 worldPosition;
    glm::vec4 color;
    glm::vec2 uv;
    glm::vec4 params;
    float z = 0.0f; // clip-space depth (gl_Position.z)
  };

  struct ShadowVertex
  {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec4 color;
    float z = 0.0f; // clip-space depth (gl_Position.z)
  };

  struct ParticleVertex
  {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec4 color;
    float z = 0.0f; // clip-space depth (gl_Position.z)
  };

private:
  int windowWidth = 0;
  int windowHeight = 0;

  // Precomputed 2 / window size so toNdc() multiplies instead of dividing
  // (it runs once per vertex, ~12k times a frame).
  float m_ndcScaleX = 0.0f;
  float m_ndcScaleY = 0.0f;

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
  GLint uLightGroundLevelsLocation = -1;
  int uSurfaceEffectLocation = -1;
  int uSurfaceEffectTimeLocation = -1;

  // Terrain heightmap used to occlude point lights against terrain. The grid is
  // re-stamped every frame (a sparse upload landing after a gap of idle frames
  // glitches the sampled texture for one frame on the macOS GL driver). To keep
  // that per-frame upload cheap it ring-buffers across several textures: writing
  // the texture the GPU is still sampling from the previous frame's draws stalls
  // the CPU until those reads drain (~a full frame), so each frame uploads into
  // the next slot in the ring -- one the GPU finished with frames ago -- and
  // binds that slot. m_heightmapScratch holds the float conversion so the upload
  // doesn't allocate each time.
  static constexpr int kHeightmapRingSize = 3;
  unsigned int heightmapTextures[kHeightmapRingSize] = {};
  int m_heightmapRing = 0; // slot written/bound this frame
  std::vector<float> m_heightmapScratch;
  int m_heightmapTexSize = 0; // allocated texture dimension (fixed, >= grid)
  int m_heightmapWidth = 0;
  int m_heightmapHeight = 0;
  int m_heightmapOriginX = 0;
  int m_heightmapOriginY = 0;
  float m_heightScale = 0.0f;
  int uHeightmapLocation = -1;
  int uHeightmapOriginLocation = -1;
  int uHeightmapSizeLocation = -1;
  int uHeightmapTexSizeLocation = -1;
  int uHeightScaleLocation = -1;

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

  PointLightSet m_pointLights;

  std::unordered_map<std::string, unsigned int> textureCache;

  // Batched

  Pipeline m_pipeline = Pipeline::None;

  unsigned int solidShaderProgram = 0;
  unsigned int solidVao = 0;
  unsigned int solidVbo = 0;

  unsigned int spriteShadowShaderProgram = 0;
  unsigned int spriteShadowVao = 0;
  unsigned int spriteShadowVbo = 0;
  int uSpriteShadowTextureLocation = -1;

  unsigned int particleShaderProgram = 0;
  unsigned int particleVao = 0;
  unsigned int particleVbo = 0;
  int uParticleTextureLocation = -1;

  std::vector<SolidVertex> m_solidVertices;
  std::vector<Vertex> m_litVertices;
  std::optional<LitBatchKey> m_litBatchKey;

  // Sprite shadows are uniform black translucent quads, so their blend order is
  // irrelevant. Bucket by texture and draw one batch per shadow atlas.
  std::unordered_map<unsigned int, std::vector<ShadowVertex>>
      m_spriteShadowBatches;

  // Particles bucketed by (texture, blend mode, depthTested); one draw call per
  // bucket. A std::map keeps the bucket set tiny and avoids needing a hash.
  std::map<std::tuple<unsigned int, BlendMode, bool>,
           std::vector<ParticleVertex>>
      m_particleBatches;
};

} // namespace sfs
