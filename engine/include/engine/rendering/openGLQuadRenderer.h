#pragma once

#include "SDL2/SDL_rect.h"
#include "SDL2/SDL_surface.h"
#include "engine/rendering/batchKeys/LitQuadBatchKey.h"
#include "engine/rendering/iQuadRenderer.h"
#include "engine/rendering/quads.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <cstddef>
#include <map>
#include <optional>
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

/**
 * OpenGL backend for the core 2D quad renderer (IQuadRenderer). Owns the solid,
 * lit, and particle pipelines, immediate textured/free-form draws, texture
 * management, and the frame lifecycle. Carries an optional terrain heightmap
 * used to occlude point lights against terrain; the heightmap is inert until an
 * isometric subclass uploads one, so a flat-2D game uses the backend without
 * it.
 */
class OpenGLQuadRenderer : public virtual IQuadRenderer
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

  void submitParticleBatch(const ParticleBatch& batch,
                           unsigned int texture,
                           BlendMode blend,
                           bool depthTested) override;

  void drawImmediate(const TexturedQuad& command) override; // Text, UI, sprites

  void begin() override;
  void flush() override;

  void
  drawLineLoop(const glm::vec2* points, int count, SDL_Color color) override;

  void setViewportSize(int width, int height) override;

  void setSurfaceTime(float time) override;

  void setPointLights(const PointLightSet& lights) override;

protected:
  enum class Pipeline
  {
    None,
    SolidColor,
    TerrainShadow,
    SpriteShadow,
    Textured,
    Freeform,
    LitSprite,
    Particle,
    Decal,
    Geometry
  };

  void beginPipeline(Pipeline stage);

  // Flush the batch queued for the current pipeline. Subclasses override to
  // handle their own pipelines, delegating the core cases here.
  virtual void flushCurrentPipeline();

  glm::vec2 toNdc(const glm::vec2& pixelPosition) const;

  unsigned int compileShader(unsigned int type, const char* source) const;

  void appendSolidVertices(const Quad& command);

  // Binds the terrain heightmap to texture unit 2 and pushes its uniforms on
  // the currently-bound program. Every path that does point-light occlusion
  // must call this; otherwise the shader samples the heightmap from whatever
  // state a prior draw happened to leave behind.
  void bindHeightmapUniforms();

  /**
   * Upload a terrain elevation grid used to occlude point lights and the sun
   * against terrain.
   *
   * @param elevations  row-major grid of integer elevation levels from the
   * origin
   * @param width       grid width in tiles (<= 0 keeps the previous grid)
   * @param height      grid height in tiles
   * @param originX     world tile x of the grid origin
   * @param originY     world tile y of the grid origin
   * @param heightScale converts an emitter's height into elevation levels
   */
  void uploadHeightmap(const int* elevations,
                       int width,
                       int height,
                       int originX,
                       int originY,
                       float heightScale);

  struct SolidVertex
  {
    glm::vec2 position;
    glm::vec4 color;
    float z = 0.0f; // clip-space depth (gl_Position.z)
  };

  Pipeline m_pipeline = Pipeline::None;

  int windowWidth = 0;
  int windowHeight = 0;

  // Precomputed 2 / window size so toNdc() multiplies instead of dividing
  // (it runs once per vertex, ~12k times a frame).
  float m_ndcScaleX = 0.0f;
  float m_ndcScaleY = 0.0f;

  bool initialized = false;

  PointLightSet m_pointLights;

  // Solid color pipeline (terrain shadows, debug fills). Shared with the iso
  // subclass, which draws projected terrain shadows through it.
  unsigned int solidShaderProgram = 0;
  unsigned int solidVao = 0;
  unsigned int solidVbo = 0;
  std::vector<SolidVertex> m_solidVertices;

  // Terrain heightmap used to occlude point lights against terrain. The grid is
  // re-stamped every frame (a sparse upload landing after a gap of idle frames
  // glitches the sampled texture for one frame on the macOS GL driver). To keep
  // that per-frame upload cheap it ring-buffers across several textures:
  // writing the texture the GPU is still sampling from the previous frame's
  // draws stalls the CPU until those reads drain (~a full frame), so each frame
  // uploads into the next slot in the ring -- one the GPU finished with frames
  // ago -- and binds that slot. m_heightmapScratch holds the float conversion
  // so the upload doesn't allocate each time.
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

  // Animation time fed to time-driven surface shaders.
  float m_surfaceTime = 0.0f;

private:
  struct Vertex
  {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec2 worldPosition;
    float z = 0.0f; // clip-space depth (gl_Position.z)
  };

  struct ParticleVertex
  {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec4 color;
    float z = 0.0f; // clip-space depth (gl_Position.z)
  };

  unsigned int createShaderProgram() const;
  unsigned int createSolidShaderProgram() const;
  unsigned int createParticleShaderProgram() const;

  void flushSolid();
  void flushLit();
  void flushParticles();

  void appendLitVertices(const LitQuad& command);
  void appendParticleVertices(const ParticleBatch& batch,
                              unsigned int texture,
                              BlendMode blend,
                              bool depthTested);

  void drawQuad(const FreeformQuad& command); // Shadows

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

  int uHeightmapLocation = -1;
  int uHeightmapOriginLocation = -1;
  int uHeightmapSizeLocation = -1;
  int uHeightmapTexSizeLocation = -1;
  int uHeightScaleLocation = -1;

  std::unordered_map<std::string, unsigned int> textureCache;

  unsigned int particleShaderProgram = 0;
  unsigned int particleVao = 0;
  unsigned int particleVbo = 0;
  int uParticleTextureLocation = -1;

  std::vector<Vertex> m_litVertices;
  std::optional<LitBatchKey> m_litBatchKey;

  // Particles bucketed by (texture, blend mode, depthTested); one draw call per
  // bucket. A std::map keeps the bucket set tiny and avoids needing a hash.
  std::map<std::tuple<unsigned int, BlendMode, bool>,
           std::vector<ParticleVertex>>
      m_particleBatches;
};

} // namespace sfs
