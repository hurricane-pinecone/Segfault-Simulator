#pragma once

#include "engine/core/rendering/quads.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/gl/glDebug.h"
#include "engine/runtime/rendering/gl/openGLQuadRenderer.h"
#include "engine/runtime/rendering/iIsometricRenderer.h"
#include "engine/runtime/rendering/util/renderStats.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <cstddef>
#include <cstdint>
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
 * OpenGL backend for the isometric heightfield path. Extends the core quad
 * renderer with the isometric extension (IIsometricRenderer): the elevation
 * heightmap, block-face geometry, sun shadows, the projected terrain/sprite
 * shadow pipelines, and persistent world-projected decals.
 */
class IsometricGeometryRenderer : public OpenGLQuadRenderer,
                                  public IIsometricRenderer
{
public:
  IsometricGeometryRenderer(int windowWidth, int windowHeight);
  ~IsometricGeometryRenderer() override;

  bool initialize() override;
  void shutdown() override;

  using OpenGLQuadRenderer::submit;

  void submit(const SurfaceCommand& command) override;

  void drawGeometry(const GeometryVertex* vertices,
                    std::size_t count,
                    unsigned int texture,
                    int surfaceEffect) override;
  void setGeometryLighting(float ambient,
                           glm::vec3 lightColor,
                           glm::vec3 sunDirection,
                           float diffuseStrength) override;

  void setSunShadowStyle(SunShadowStyle style) override
  {
    m_sunShadowStyle = style;
  }

  void setSunShadowMarchEnabled(bool enabled) override
  {
    m_sunShadowMarchEnabled = enabled;
  }

  void submitTerrainShadow(const Quad& command) override
  {
    initialize();

    if (!initialized)
      return;

    beginPipeline(Pipeline::TerrainShadow);
    appendSolidVertices(command);
  }

  void submitSpriteShadow(const FreeformQuad& command) override;

  void setDecalFrameParams(const DecalFrameParams& params) override;
  void bakeDecals(std::int64_t key,
                  int texW,
                  int texH,
                  unsigned int sprite,
                  const DecalBakeVertex* verts,
                  std::size_t count) override;
  void uploadPaintDraw(std::int64_t key,
                       const DecalVertex* verts,
                       std::size_t count) override;
  void freePaintTarget(std::int64_t key) override;
  void drawPaintTarget(std::int64_t key) override;
  void drawDecalsDynamic(const DecalVertex* vertices,
                         std::size_t count,
                         unsigned int texture) override;

  void setHeightmap(const int* elevations,
                    int width,
                    int height,
                    int originX,
                    int originY,
                    float heightScale) override
  {
    uploadHeightmap(elevations, width, height, originX, originY, heightScale);
  }

protected:
  void flushCurrentPipeline() override;

  // Complete the lit shader with the heightfield/terrain hook impls (terrain
  // sun shadows, point-light terrain occlusion, iso surface effects).
  std::string litShaderImpl() const override;

private:
  unsigned int createSurfaceShaderProgram() const;
  unsigned int createSpriteShadowShaderProgram() const;
  unsigned int createDecalShaderProgram() const;
  unsigned int createDecalBakeShaderProgram() const;
  unsigned int createGeometryShaderProgram() const;

  // Bind the geometry program + opaque state + frame
  // lighting/point-light/heightmap uniforms (flushing any pending pipeline
  // first).
  void beginGeometryPipeline();

  void flushSpriteShadow();

  // Bind the decal program + state (flushing any pending pipeline first).
  // Decals draw immediately from persistent/dynamic buffers, so there's nothing
  // to accumulate -- this just makes the decal pipeline current once.
  void beginDecalPipeline();

  // Set the DecalVertex attribute layout on the currently-bound VAO+VBO (shared
  // by the dynamic buffer and every persistent chunk buffer).
  void configureDecalAttribs();

  void appendSpriteShadowVertices(const FreeformQuad& command);

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

    gTerrainShadowFlushes++;
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_solidVertices.size()));
    SFS_GL_CHECK("terrainShadowFlush");

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

private:
  unsigned int surfaceShaderProgram = 0;
  unsigned int surfaceVao = 0;
  unsigned int surfaceVbo = 0;
  unsigned int surfaceEbo = 0;

  int uSurfaceTimeLocation = -1;
  int uSurfaceWaveStrengthLocation = -1;
  int uSurfaceWorldToClipXLocation = -1;
  int uSurfaceWorldToClipYLocation = -1;
  int uSurfaceWorldToClipELocation = -1;
  int uSurfaceRippleStrengthLocation = -1;
  int uSurfaceAmbientLocation = -1;
  int uSurfaceLightCountLocation = -1;
  int uSurfaceLightPositionsLocation = -1;
  int uSurfaceLightColorsLocation = -1;
  int uSurfaceLightIntensitiesLocation = -1;
  int uSurfaceLightRadiiLocation = -1;

  unsigned int spriteShadowShaderProgram = 0;
  unsigned int spriteShadowVao = 0;
  unsigned int spriteShadowVbo = 0;
  int uSpriteShadowTextureLocation = -1;

  // Sprite shadows are uniform black translucent quads, so their blend order is
  // irrelevant. Bucket by texture and draw one batch per shadow atlas.
  std::unordered_map<unsigned int, std::vector<ShadowVertex>>
      m_spriteShadowBatches;

  // --- Baked decals ---
  // Permanent stains are baked into per-target paint textures (ground: one per
  // chunk's world-XY footprint; walls: one per face). The draw geometry is a
  // fixed quad per painted tile/face that samples the texture, so memory is
  // bounded by painted area, not by spray count. Still-animating decals (fading
  // water, running drips) bypass baking and draw from the dynamic buffer.
  struct PaintTarget
  {
    unsigned int tex = 0; // RGBA8 paint texture (FBO colour attachment to bake)
    int texW = 0;
    int texH = 0;
    unsigned int vao = 0; // persistent draw-quad buffer (DecalVertex)
    unsigned int vbo = 0;
    int count = 0;    // draw vertices in use
    int capacity = 0; // draw vertices the VBO can hold before it must grow
  };

  // Look up a target; returns nullptr if it doesn't exist yet.
  PaintTarget* findPaintTarget(std::int64_t key);
  // Ensure the target's paint texture exists (sized texW x texH, cleared
  // transparent); returns the target.
  PaintTarget& ensurePaintTexture(std::int64_t key, int texW, int texH);

  unsigned int decalShaderProgram = 0;
  unsigned int decalDynamicVao = 0;
  unsigned int decalDynamicVbo = 0;

  // Bake pass: one shared FBO whose colour attachment is swapped to the target
  // texture per bake, a tiny program that stamps the sprite, and a transient
  // upload buffer for the bake vertices.
  unsigned int decalBakeProgram = 0;
  unsigned int decalBakeFbo = 0;
  unsigned int decalBakeVao = 0;
  unsigned int decalBakeVbo = 0;
  int uDecalBakeSpriteLocation = -1;

  int uDecalTextureLocation = -1;
  int uDecalTileSizeLocation = -1; // (tileWidth, tileHeight)
  int uDecalWorldScaleLocation = -1;
  int uDecalZoomLocation = -1;
  int uDecalCameraIsoLocation = -1;
  int uDecalScreenCenterLocation = -1;
  int uDecalElevationStepLocation = -1;
  int uDecalNdcScaleLocation = -1;
  int uDecalDepthMinLocation = -1;
  int uDecalDepthInvRangeLocation = -1;
  // lighting (ambient + the frame's point lights, same data as the lit shader)
  int uDecalAmbientLocation = -1;
  int uDecalAmbientColorLocation = -1;
  int uDecalHeightScaleLocation = -1;
  int uDecalLightCountLocation = -1;
  int uDecalLightPositionsLocation = -1;
  int uDecalLightColorsLocation = -1;
  int uDecalLightIntensitiesLocation = -1;
  int uDecalLightRadiiLocation = -1;
  int uDecalLightHeightsLocation = -1;
  int uDecalLightGroundLevelsLocation = -1;

  DecalFrameParams m_decalParams;
  std::unordered_map<std::int64_t, PaintTarget> m_paintTargets;

  // --- Opt-in block geometry (real terrain faces) ---
  // A lit, textured face mesh built per frame by the BlockGeometry module. Uses
  // the frame's point lights + heightmap (already on the renderer) and a real
  // per-vertex normal + per-vertex elevation, so side faces light from the
  // base.
  unsigned int geometryShaderProgram = 0;
  unsigned int geometryVao = 0;
  unsigned int geometryVbo = 0;

  float m_geomAmbient = 1.0f;
  glm::vec3 m_geomLightColor{1.0f, 1.0f, 1.0f};
  glm::vec3 m_geomSunDirection{0.0f, 0.0f, 1.0f};
  float m_geomDiffuseStrength = 0.0f;

  std::vector<GeometryVertex> m_geometryScratch; // NDC-converted upload buffer

  // Sun-shadow sampling style for the geometry path; defaults to the soft look.
  SunShadowStyle m_sunShadowStyle = SunShadowStyle::Smooth;
};

} // namespace sfs
