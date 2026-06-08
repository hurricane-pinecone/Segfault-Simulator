
#include "engine/runtime/rendering/gl/isometricGeometryRenderer.h"

#include "engine/core/logger/logger.h"
#include "engine/core/rendering/quads.h"
#include "engine/core/util/profiling.h"
#include "engine/generated/embeddedShaders.h"
#include "engine/runtime/rendering/gl/glDebug.h"
#include "engine/runtime/rendering/gl/gpuProfiling.h"
#include "engine/runtime/systems/isometric/isometricRenderSystem.h"
#include <algorithm>

#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

#include <SDL_surface.h>

#include <cstddef>
#include <string>
#include <vector>

namespace sfs
{

IsometricGeometryRenderer::IsometricGeometryRenderer(int windowWidth,
                                                     int windowHeight)
    : OpenGLQuadRenderer(windowWidth, windowHeight)
{
}

IsometricGeometryRenderer::~IsometricGeometryRenderer() { shutdown(); }

bool IsometricGeometryRenderer::initialize()
{
  if (initialized)
    return true;

  // Core pipelines (lit, solid, particle), textures, heightmap ring, and the
  // shared GL state come up first.
  if (!OpenGLQuadRenderer::initialize())
    return false;

  // ===========================================================================
  // Surface shader.
  // Used by:
  // - water rendering
  // - animated surface effects
  // ===========================================================================

  surfaceShaderProgram = createSurfaceShaderProgram();

  if (surfaceShaderProgram == 0)
  {
    LOG_ERROR(
        "Failed to create IsometricGeometryRenderer surface shader program");
    return false;
  }

  // ===========================================================================
  // Textured shadow shader.
  // Used by:
  // - projected sprite shadows (silhouette alpha from the sprite, per-vertex
  //   tint), batched by texture
  // ===========================================================================

  spriteShadowShaderProgram = createSpriteShadowShaderProgram();

  if (spriteShadowShaderProgram == 0)
  {
    LOG_ERROR(
        "Failed to create IsometricGeometryRenderer sprite shadow shader");
    return false;
  }

  uSpriteShadowTextureLocation =
      SFS_GL_UNIFORM(spriteShadowShaderProgram, "uTexture");

  // ===========================================================================
  // Surface shader uniform locations
  // ===========================================================================

  uSurfaceTimeLocation = SFS_GL_UNIFORM(surfaceShaderProgram, "uTime");
  uSurfaceWaveStrengthLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uWaveStrength");
  uSurfaceWorldToClipXLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uWorldToClipX");
  uSurfaceWorldToClipYLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uWorldToClipY");
  uSurfaceWorldToClipELocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uWorldToClipE");
  uSurfaceRippleStrengthLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uRippleStrength");
  uSurfaceAmbientLocation = SFS_GL_UNIFORM(surfaceShaderProgram, "uAmbient");
  uSurfaceLightCountLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uLightCount");
  uSurfaceLightPositionsLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uLightPositions[0]");
  uSurfaceLightColorsLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uLightColors[0]");
  uSurfaceLightIntensitiesLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uLightIntensities[0]");
  uSurfaceLightRadiiLocation =
      SFS_GL_UNIFORM(surfaceShaderProgram, "uLightRadii[0]");

  // ===========================================================================
  // Surface rendering VAO/VBO/EBO
  // Used by water/surface meshes.
  // ===========================================================================

  glGenVertexArrays(1, &surfaceVao);
  glGenBuffers(1, &surfaceVbo);
  glGenBuffers(1, &surfaceEbo);

  glBindVertexArray(surfaceVao);
  glBindBuffer(GL_ARRAY_BUFFER, surfaceVbo);

  // position
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(SurfaceGpuVertex),
      reinterpret_cast<void*>(offsetof(SurfaceGpuVertex, position)));

  // world position
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
      1,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(SurfaceGpuVertex),
      reinterpret_cast<void*>(offsetof(SurfaceGpuVertex, worldPosition)));

  // color
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(
      2,
      4,
      GL_FLOAT,
      GL_FALSE,
      sizeof(SurfaceGpuVertex),
      reinterpret_cast<void*>(offsetof(SurfaceGpuVertex, color)));

  // uv
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(
      3,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(SurfaceGpuVertex),
      reinterpret_cast<void*>(offsetof(SurfaceGpuVertex, uv)));

  // params
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(
      4,
      4,
      GL_FLOAT,
      GL_FALSE,
      sizeof(SurfaceGpuVertex),
      reinterpret_cast<void*>(offsetof(SurfaceGpuVertex, params)));

  // clip-space depth
  glEnableVertexAttribArray(5);
  glVertexAttribPointer(5,
                        1,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(SurfaceGpuVertex),
                        reinterpret_cast<void*>(offsetof(SurfaceGpuVertex, z)));

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, surfaceEbo);

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  // ===========================================================================
  // Sprite shadow VAO/VBO
  // Textured (silhouette) + per-vertex tint, batched by texture.
  // ===========================================================================

  glGenVertexArrays(1, &spriteShadowVao);
  glGenBuffers(1, &spriteShadowVbo);

  glBindVertexArray(spriteShadowVao);
  glBindBuffer(GL_ARRAY_BUFFER, spriteShadowVbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(ShadowVertex) * 6),
               nullptr,
               GL_DYNAMIC_DRAW);

  // position
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(ShadowVertex),
      reinterpret_cast<void*>(offsetof(ShadowVertex, position)));

  // uv
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(ShadowVertex),
                        reinterpret_cast<void*>(offsetof(ShadowVertex, uv)));

  // color (tint)
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,
                        4,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(ShadowVertex),
                        reinterpret_cast<void*>(offsetof(ShadowVertex, color)));

  // clip-space depth
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3,
                        1,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(ShadowVertex),
                        reinterpret_cast<void*>(offsetof(ShadowVertex, z)));

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // ===========================================================================
  // Decal pipeline (persistent world-space stain buffers, GPU-projected)
  // ===========================================================================

  decalShaderProgram = createDecalShaderProgram();

  if (decalShaderProgram == 0)
  {
    LOG_ERROR("Failed to create IsometricGeometryRenderer decal shader");
    return false;
  }

  uDecalTextureLocation = SFS_GL_UNIFORM(decalShaderProgram, "uTexture");
  uDecalTileSizeLocation = SFS_GL_UNIFORM(decalShaderProgram, "uTileSize");
  uDecalWorldScaleLocation = SFS_GL_UNIFORM(decalShaderProgram, "uWorldScale");
  uDecalZoomLocation = SFS_GL_UNIFORM(decalShaderProgram, "uZoom");
  uDecalCameraIsoLocation = SFS_GL_UNIFORM(decalShaderProgram, "uCameraIso");
  uDecalScreenCenterLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uScreenCenter");
  uDecalElevationStepLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uElevationStep");
  uDecalNdcScaleLocation = SFS_GL_UNIFORM(decalShaderProgram, "uNdcScale");
  uDecalDepthMinLocation = SFS_GL_UNIFORM(decalShaderProgram, "uDepthMin");
  uDecalDepthInvRangeLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uDepthInvRange");
  uDecalAmbientLocation = SFS_GL_UNIFORM(decalShaderProgram, "uAmbient");
  uDecalAmbientColorLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uAmbientColor");
  uDecalHeightScaleLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uHeightScale");
  uDecalLightCountLocation = SFS_GL_UNIFORM(decalShaderProgram, "uLightCount");
  uDecalLightPositionsLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uLightPositions[0]");
  uDecalLightColorsLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uLightColors[0]");
  uDecalLightIntensitiesLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uLightIntensities[0]");
  uDecalLightRadiiLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uLightRadii[0]");
  uDecalLightHeightsLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uLightHeights[0]");
  uDecalLightGroundLevelsLocation =
      SFS_GL_UNIFORM(decalShaderProgram, "uLightGroundLevels[0]");

  glGenVertexArrays(1, &decalDynamicVao);
  glGenBuffers(1, &decalDynamicVbo);
  glBindVertexArray(decalDynamicVao);
  glBindBuffer(GL_ARRAY_BUFFER, decalDynamicVbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(DecalVertex) * 6),
               nullptr,
               GL_DYNAMIC_DRAW);
  configureDecalAttribs();
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // ---------------------------------------------------------------------------
  // Decal bake pass: stamps permanent splats into per-target paint textures via
  // one shared FBO whose colour attachment is swapped to the target per bake.
  // ---------------------------------------------------------------------------
  decalBakeProgram = createDecalBakeShaderProgram();

  if (decalBakeProgram == 0)
  {
    LOG_ERROR("Failed to create IsometricGeometryRenderer decal bake shader");
    return false;
  }

  uDecalBakeSpriteLocation = SFS_GL_UNIFORM(decalBakeProgram, "uSprite");

  glGenFramebuffers(1, &decalBakeFbo);

  glGenVertexArrays(1, &decalBakeVao);
  glGenBuffers(1, &decalBakeVbo);
  glBindVertexArray(decalBakeVao);
  glBindBuffer(GL_ARRAY_BUFFER, decalBakeVbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(DecalBakeVertex) * 6),
               nullptr,
               GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0); // local [0,1]
  glVertexAttribPointer(
      0,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(DecalBakeVertex),
      reinterpret_cast<void*>(offsetof(DecalBakeVertex, local)));
  glEnableVertexAttribArray(1); // sprite uv
  glVertexAttribPointer(1,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(DecalBakeVertex),
                        reinterpret_cast<void*>(offsetof(DecalBakeVertex, uv)));
  glEnableVertexAttribArray(2); // packed RGBA8 colour
  glVertexAttribPointer(
      2,
      4,
      GL_UNSIGNED_BYTE,
      GL_TRUE,
      sizeof(DecalBakeVertex),
      reinterpret_cast<void*>(offsetof(DecalBakeVertex, color)));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // ===========================================================================
  // Block geometry pipeline (opt-in real terrain faces)
  // ===========================================================================

  geometryShaderProgram = createGeometryShaderProgram();

  if (geometryShaderProgram == 0)
  {
    LOG_ERROR("Failed to create IsometricGeometryRenderer geometry shader");
    return false;
  }

  glGenVertexArrays(1, &geometryVao);
  glGenBuffers(1, &geometryVbo);
  glBindVertexArray(geometryVao);
  glBindBuffer(GL_ARRAY_BUFFER, geometryVbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(GeometryVertex) * 3),
               nullptr,
               GL_DYNAMIC_DRAW);
  // position
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(GeometryVertex),
      reinterpret_cast<void*>(offsetof(GeometryVertex, position)));
  // worldPos
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
      1,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(GeometryVertex),
      reinterpret_cast<void*>(offsetof(GeometryVertex, worldPos)));
  // ground (elevation)
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(
      2,
      1,
      GL_FLOAT,
      GL_FALSE,
      sizeof(GeometryVertex),
      reinterpret_cast<void*>(offsetof(GeometryVertex, ground)));
  // uv
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(GeometryVertex),
                        reinterpret_cast<void*>(offsetof(GeometryVertex, uv)));
  // normal
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(
      4,
      3,
      GL_FLOAT,
      GL_FALSE,
      sizeof(GeometryVertex),
      reinterpret_cast<void*>(offsetof(GeometryVertex, normal)));
  // clip-space depth
  glEnableVertexAttribArray(5);
  glVertexAttribPointer(5,
                        1,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(GeometryVertex),
                        reinterpret_cast<void*>(offsetof(GeometryVertex, z)));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // ===========================================================================
  // Default uniforms: surface shader
  // ===========================================================================

  glUseProgram(surfaceShaderProgram);
  glUniform1f(uSurfaceTimeLocation, 0.0f);
  glUniform1f(uSurfaceRippleStrengthLocation, 0.025f);

  // Water/surface default ambient.
  glUniform1f(uSurfaceAmbientLocation, 1.0f);

  glUseProgram(0);

  SFS_GL_CHECK("IsometricGeometryRenderer::initialize");

  return true;
}

void IsometricGeometryRenderer::shutdown()
{
  if (!initialized)
    return;

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  if (spriteShadowVbo != 0)
    glDeleteBuffers(1, &spriteShadowVbo);

  if (spriteShadowVao != 0)
    glDeleteVertexArrays(1, &spriteShadowVao);

  if (spriteShadowShaderProgram != 0)
    glDeleteProgram(spriteShadowShaderProgram);

  for (auto& [key, target] : m_paintTargets)
  {
    if (target.tex != 0)
      glDeleteTextures(1, &target.tex);
    if (target.vbo != 0)
      glDeleteBuffers(1, &target.vbo);
    if (target.vao != 0)
      glDeleteVertexArrays(1, &target.vao);
  }
  m_paintTargets.clear();

  if (decalBakeFbo != 0)
    glDeleteFramebuffers(1, &decalBakeFbo);
  if (decalBakeVbo != 0)
    glDeleteBuffers(1, &decalBakeVbo);
  if (decalBakeVao != 0)
    glDeleteVertexArrays(1, &decalBakeVao);
  if (decalBakeProgram != 0)
    glDeleteProgram(decalBakeProgram);

  if (decalDynamicVbo != 0)
    glDeleteBuffers(1, &decalDynamicVbo);
  if (decalDynamicVao != 0)
    glDeleteVertexArrays(1, &decalDynamicVao);
  if (decalShaderProgram != 0)
    glDeleteProgram(decalShaderProgram);

  if (geometryVbo != 0)
    glDeleteBuffers(1, &geometryVbo);
  if (geometryVao != 0)
    glDeleteVertexArrays(1, &geometryVao);
  if (geometryShaderProgram != 0)
    glDeleteProgram(geometryShaderProgram);

  if (surfaceEbo != 0)
    glDeleteBuffers(1, &surfaceEbo);

  if (surfaceVbo != 0)
    glDeleteBuffers(1, &surfaceVbo);

  if (surfaceVao != 0)
    glDeleteVertexArrays(1, &surfaceVao);

  if (surfaceShaderProgram != 0)
    glDeleteProgram(surfaceShaderProgram);

  surfaceEbo = 0;
  surfaceVbo = 0;
  surfaceVao = 0;
  surfaceShaderProgram = 0;

  spriteShadowVbo = 0;
  spriteShadowVao = 0;
  spriteShadowShaderProgram = 0;
  m_spriteShadowBatches.clear();

  decalDynamicVbo = 0;
  decalDynamicVao = 0;
  decalShaderProgram = 0;

  decalBakeFbo = 0;
  decalBakeVbo = 0;
  decalBakeVao = 0;
  decalBakeProgram = 0;

  geometryVbo = 0;
  geometryVao = 0;
  geometryShaderProgram = 0;

  // Releases the core pipelines, textures, and clears initialized.
  OpenGLQuadRenderer::shutdown();
}

void IsometricGeometryRenderer::submit(const SurfaceCommand& command)
{
  ZoneScopedN("GL: submit surface");
  TracyGpuZone("GPU: surface");

  initialize();

  if (!initialized)
    return;

  if (command.vertices.empty() || command.indices.empty())
    return;

  if (surfaceShaderProgram == 0)
    return;

  flushCurrentPipeline();
  m_pipeline = Pipeline::None;

  std::vector<SurfaceGpuVertex> gpuVertices;
  gpuVertices.reserve(command.vertices.size());

  for (const SurfaceVertex& vertex : command.vertices)
  {
    gpuVertices.push_back(SurfaceGpuVertex{
        toNdc(vertex.position),
        vertex.worldPosition,
        vertex.color,
        vertex.uv,
        vertex.params,
        vertex.z,
    });
  }

  glUseProgram(surfaceShaderProgram);

  // Water is translucent: test against the opaque depth (cliffs/blocks occlude
  // water behind them) but do not write depth.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUniform1f(uSurfaceTimeLocation, m_surfaceTime);
  // The command's basis is a SCREEN-PIXEL delta; the vertices reach the shader
  // in NDC (toNdc), so convert the delta the same way: dx*scaleX, -dy*scaleY
  // (toNdc flips y). Without this the wave displacement is ~viewport-sized.
  glUniform2f(uSurfaceWorldToClipXLocation,
              command.worldToClipX.x * m_ndcScaleX,
              -command.worldToClipX.y * m_ndcScaleY);
  glUniform2f(uSurfaceWorldToClipYLocation,
              command.worldToClipY.x * m_ndcScaleX,
              -command.worldToClipY.y * m_ndcScaleY);
  glUniform2f(uSurfaceWorldToClipELocation,
              command.worldToClipE.x * m_ndcScaleX,
              -command.worldToClipE.y * m_ndcScaleY);
  glUniform1f(uSurfaceWaveStrengthLocation, command.waveStrength);
  glUniform1f(uSurfaceRippleStrengthLocation, command.rippleStrength);
  glUniform1f(uSurfaceAmbientLocation, command.ambient);
  glUniform1i(uSurfaceLightCountLocation, m_pointLights.count);

  if (m_pointLights.count > 0)
  {
    glUniform2fv(uSurfaceLightPositionsLocation,
                 m_pointLights.count,
                 reinterpret_cast<const float*>(m_pointLights.positions));
    glUniform3fv(uSurfaceLightColorsLocation,
                 m_pointLights.count,
                 reinterpret_cast<const float*>(m_pointLights.colors));
    glUniform1fv(uSurfaceLightIntensitiesLocation,
                 m_pointLights.count,
                 m_pointLights.intensities);
    glUniform1fv(
        uSurfaceLightRadiiLocation, m_pointLights.count, m_pointLights.radii);
  }

  // Sun shadow: the same heightmap + sun direction the geometry/decal shaders
  // use, so a cliff/mountain casts its shadow onto the water surface (same
  // horizon march). Locations looked up inline like the decal path.
  glUniform3f(SFS_GL_UNIFORM(surfaceShaderProgram, "uLightDirection"),
              m_geomSunDirection.x,
              m_geomSunDirection.y,
              m_geomSunDirection.z);
  glUniform1f(SFS_GL_UNIFORM(surfaceShaderProgram, "uDiffuseStrength"),
              m_geomDiffuseStrength);
  glUniform1i(SFS_GL_UNIFORM(surfaceShaderProgram, "uSunShadowEnabled"),
              m_sunShadowMarchEnabled ? 1 : 0);
  // (water always uses the smooth march, so no uShadowSharp here.)

  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, heightmapTextures[m_heightmapRing]);
  glUniform1i(SFS_GL_UNIFORM(surfaceShaderProgram, "uHeightmap"), 2);
  glUniform2f(SFS_GL_UNIFORM(surfaceShaderProgram, "uHeightmapOrigin"),
              static_cast<float>(m_heightmapOriginX),
              static_cast<float>(m_heightmapOriginY));
  glUniform2f(SFS_GL_UNIFORM(surfaceShaderProgram, "uHeightmapSize"),
              static_cast<float>(m_heightmapWidth),
              static_cast<float>(m_heightmapHeight));
  glUniform1f(SFS_GL_UNIFORM(surfaceShaderProgram, "uHeightmapTexSize"),
              static_cast<float>(m_heightmapTexSize));
  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(surfaceVao);

  glBindBuffer(GL_ARRAY_BUFFER, surfaceVbo);
  glBufferData(
      GL_ARRAY_BUFFER,
      static_cast<GLsizeiptr>(gpuVertices.size() * sizeof(SurfaceGpuVertex)),
      gpuVertices.data(),
      GL_DYNAMIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, surfaceEbo);
  glBufferData(
      GL_ELEMENT_ARRAY_BUFFER,
      static_cast<GLsizeiptr>(command.indices.size() * sizeof(uint32_t)),
      command.indices.data(),
      GL_DYNAMIC_DRAW);

  glDrawElements(GL_TRIANGLES,
                 static_cast<GLsizei>(command.indices.size()),
                 GL_UNSIGNED_INT,
                 nullptr);
  SFS_GL_CHECK("surfaceFlush");

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);
}

void IsometricGeometryRenderer::submitSpriteShadow(const FreeformQuad& command)
{
  initialize();

  if (!initialized || command.texture == 0)
    return;

  if (m_pipeline != Pipeline::SpriteShadow)
  {
    flushCurrentPipeline();
    m_pipeline = Pipeline::SpriteShadow;
  }

  appendSpriteShadowVertices(command);
}

void IsometricGeometryRenderer::appendSpriteShadowVertices(
    const FreeformQuad& command)
{
  const glm::vec2 p0 = toNdc(command.points[0]);
  const glm::vec2 p1 = toNdc(command.points[1]);
  const glm::vec2 p2 = toNdc(command.points[2]);
  const glm::vec2 p3 = toNdc(command.points[3]);

  const glm::vec4 color{
      command.tint.r / 255.0f,
      command.tint.g / 255.0f,
      command.tint.b / 255.0f,
      command.tint.a / 255.0f,
  };

  const float z = command.z;

  // Bucket by texture; order within/between buckets does not matter for black
  // translucent shadows.
  std::vector<ShadowVertex>& verts = m_spriteShadowBatches[command.texture];

  verts.push_back({p0, command.uvs[0], color, z});
  verts.push_back({p1, command.uvs[1], color, z});
  verts.push_back({p2, command.uvs[2], color, z});

  verts.push_back({p0, command.uvs[0], color, z});
  verts.push_back({p2, command.uvs[2], color, z});
  verts.push_back({p3, command.uvs[3], color, z});
}

void IsometricGeometryRenderer::flushSpriteShadow()
{
  ZoneScopedN("GL: flushSpriteShadow");
  TracyGpuZone("GPU: sprite shadow");

  initialize();

  if (!initialized || m_spriteShadowBatches.empty())
    return;

  glUseProgram(spriteShadowShaderProgram);

  // Translucent: test against the opaque depth (a block in front occludes the
  // shadow) but do not write depth.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glActiveTexture(GL_TEXTURE0);
  glUniform1i(uSpriteShadowTextureLocation, 0);

  glBindVertexArray(spriteShadowVao);
  glBindBuffer(GL_ARRAY_BUFFER, spriteShadowVbo);

  // One draw per shadow atlas. Black-shadow blend is order-independent, so the
  // (unordered) bucket iteration order is fine.
  for (auto& [texture, verts] : m_spriteShadowBatches)
  {
    if (verts.empty())
      continue;

    glBindTexture(GL_TEXTURE_2D, texture);

    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(ShadowVertex)),
                 verts.data(),
                 GL_DYNAMIC_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
    SFS_GL_CHECK("spriteShadow");
  }

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);

  m_spriteShadowBatches.clear();
}

void IsometricGeometryRenderer::configureDecalAttribs()
{
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(DecalVertex),
      reinterpret_cast<void*>(offsetof(DecalVertex, worldPos)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
      1,
      1,
      GL_FLOAT,
      GL_FALSE,
      sizeof(DecalVertex),
      reinterpret_cast<void*>(offsetof(DecalVertex, elevation)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(DecalVertex),
                        reinterpret_cast<void*>(offsetof(DecalVertex, uv)));
  // Colour is a packed RGBA8; GL normalises the ubyte4 back to a vec4 in [0,1].
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3,
                        4,
                        GL_UNSIGNED_BYTE,
                        GL_TRUE,
                        sizeof(DecalVertex),
                        reinterpret_cast<void*>(offsetof(DecalVertex, color)));
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(
      4,
      1,
      GL_FLOAT,
      GL_FALSE,
      sizeof(DecalVertex),
      reinterpret_cast<void*>(offsetof(DecalVertex, sortKey)));
}

void IsometricGeometryRenderer::setDecalFrameParams(
    const DecalFrameParams& params)
{
  m_decalParams = params;
}

void IsometricGeometryRenderer::beginDecalPipeline()
{
  if (m_pipeline == Pipeline::Decal)
    return;

  flushCurrentPipeline();
  m_pipeline = Pipeline::Decal;

  glUseProgram(decalShaderProgram);

  // Translucent: occluded by the opaque scene depth, but never writes depth.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUniform1i(uDecalTextureLocation, 0);
  glUniform2f(uDecalTileSizeLocation,
              m_decalParams.tileWidth,
              m_decalParams.tileHeight);
  glUniform1f(uDecalWorldScaleLocation, m_decalParams.worldScale);
  glUniform1f(uDecalZoomLocation, m_decalParams.zoom);
  glUniform2f(uDecalCameraIsoLocation,
              m_decalParams.cameraIso.x,
              m_decalParams.cameraIso.y);
  glUniform2f(uDecalScreenCenterLocation,
              m_decalParams.screenCenter.x,
              m_decalParams.screenCenter.y);
  glUniform1f(uDecalElevationStepLocation, m_decalParams.elevationStep);
  glUniform2f(uDecalNdcScaleLocation, m_ndcScaleX, m_ndcScaleY);
  glUniform1f(uDecalDepthMinLocation, m_decalParams.depthMin);
  glUniform1f(uDecalDepthInvRangeLocation, m_decalParams.depthInvRange);

  // Lighting: ambient (day/night) + the frame's point lights (same set the lit
  // shader uses), so blood darkens in the dark and lights up near lamps.
  glUniform1f(uDecalAmbientLocation, m_decalParams.ambient);
  glUniform3f(uDecalAmbientColorLocation,
              m_decalParams.ambientColor.x,
              m_decalParams.ambientColor.y,
              m_decalParams.ambientColor.z);
  glUniform1f(uDecalHeightScaleLocation, m_heightScale);
  glUniform1i(uDecalLightCountLocation, m_pointLights.count);
  if (m_pointLights.count > 0)
  {
    glUniform2fv(uDecalLightPositionsLocation,
                 m_pointLights.count,
                 &m_pointLights.positions[0].x);
    glUniform3fv(uDecalLightColorsLocation,
                 m_pointLights.count,
                 &m_pointLights.colors[0].x);
    glUniform1fv(uDecalLightIntensitiesLocation,
                 m_pointLights.count,
                 m_pointLights.intensities);
    glUniform1fv(
        uDecalLightRadiiLocation, m_pointLights.count, m_pointLights.radii);
    glUniform1fv(
        uDecalLightHeightsLocation, m_pointLights.count, m_pointLights.heights);
    glUniform1fv(uDecalLightGroundLevelsLocation,
                 m_pointLights.count,
                 m_pointLights.groundLevels);
  }

  // Sun shadow: the same sun direction + heightmap the geometry/lit shaders
  // use, so a stain darkens in the cast shadows its surface sits in. Bound to
  // the decal program here (bindHeightmapUniforms targets the lit program's
  // cached locations, so the decal looks its own up each frame like the
  // geometry path).
  glUniform3f(SFS_GL_UNIFORM(decalShaderProgram, "uLightDirection"),
              m_geomSunDirection.x,
              m_geomSunDirection.y,
              m_geomSunDirection.z);
  glUniform1f(SFS_GL_UNIFORM(decalShaderProgram, "uDiffuseStrength"),
              m_geomDiffuseStrength);
  glUniform1i(SFS_GL_UNIFORM(decalShaderProgram, "uSunShadowEnabled"),
              m_sunShadowMarchEnabled ? 1 : 0);
  glUniform1i(SFS_GL_UNIFORM(decalShaderProgram, "uShadowSharp"),
              m_sunShadowStyle == SunShadowStyle::Sharp ? 1 : 0);

  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, heightmapTextures[m_heightmapRing]);
  glUniform1i(SFS_GL_UNIFORM(decalShaderProgram, "uHeightmap"), 2);
  glUniform2f(SFS_GL_UNIFORM(decalShaderProgram, "uHeightmapOrigin"),
              static_cast<float>(m_heightmapOriginX),
              static_cast<float>(m_heightmapOriginY));
  glUniform2f(SFS_GL_UNIFORM(decalShaderProgram, "uHeightmapSize"),
              static_cast<float>(m_heightmapWidth),
              static_cast<float>(m_heightmapHeight));
  glUniform1f(SFS_GL_UNIFORM(decalShaderProgram, "uHeightmapTexSize"),
              static_cast<float>(m_heightmapTexSize));

  glActiveTexture(GL_TEXTURE0);
}

IsometricGeometryRenderer::PaintTarget*
IsometricGeometryRenderer::findPaintTarget(std::int64_t key)
{
  auto it = m_paintTargets.find(key);
  return it == m_paintTargets.end() ? nullptr : &it->second;
}

IsometricGeometryRenderer::PaintTarget&
IsometricGeometryRenderer::ensurePaintTexture(std::int64_t key,
                                              int texW,
                                              int texH)
{
  PaintTarget& target = m_paintTargets[key];
  if (target.tex == 0)
  {
    // Allocate the paint texture zero-filled (transparent), so the first splat
    // bakes onto a clean surface without needing an FBO clear.
    const std::vector<unsigned char> zeros(
        static_cast<std::size_t>(texW) * static_cast<std::size_t>(texH) * 4, 0);
    glGenTextures(1, &target.tex);
    glBindTexture(GL_TEXTURE_2D, target.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 texW,
                 texH,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 zeros.data());
    target.texW = texW;
    target.texH = texH;
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  return target;
}

void IsometricGeometryRenderer::bakeDecals(std::int64_t key,
                                           int texW,
                                           int texH,
                                           unsigned int sprite,
                                           const DecalBakeVertex* verts,
                                           std::size_t count)
{
  initialize();
  if (!initialized || count == 0 || sprite == 0 || texW <= 0 || texH <= 0)
    return;

  PaintTarget& target = ensurePaintTexture(key, texW, texH);

  // Baking renders into an offscreen texture, so flush any pending batch, then
  // capture the bound framebuffer + viewport to restore afterward.
  flushCurrentPipeline();

  GLint prevFbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
  GLint prevViewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, prevViewport);

  glBindFramebuffer(GL_FRAMEBUFFER, decalBakeFbo);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.tex, 0);
  glViewport(0, 0, target.texW, target.texH);

  glUseProgram(decalBakeProgram);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  // Premultiplied-alpha "over": the bake frag premultiplies the sprite, so the
  // same colour saturates and a new colour paints over -- never growing memory.
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, sprite);
  glUniform1i(uDecalBakeSpriteLocation, 0);

  glBindVertexArray(decalBakeVao);
  glBindBuffer(GL_ARRAY_BUFFER, decalBakeVbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(count * sizeof(DecalBakeVertex)),
               verts,
               GL_DYNAMIC_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));
  SFS_GL_CHECK("decalBake");

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // Detach the target texture and restore the previous render target/viewport.
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
  glViewport(
      prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

  // The bake swapped program/blend/FBO out from under the decal pipeline; force
  // the next decal draw to re-establish it.
  m_pipeline = Pipeline::None;
}

void IsometricGeometryRenderer::uploadPaintDraw(std::int64_t key,
                                                const DecalVertex* verts,
                                                std::size_t count)
{
  initialize();
  if (!initialized)
    return;

  // Entering the decal pipeline first flushes any pending batch, so binding
  // buffers here can't corrupt another pipeline's VAO state.
  beginDecalPipeline();

  PaintTarget& target = m_paintTargets[key];
  if (target.vao == 0)
  {
    glGenVertexArrays(1, &target.vao);
    glGenBuffers(1, &target.vbo);
    glBindVertexArray(target.vao);
    glBindBuffer(GL_ARRAY_BUFFER, target.vbo);
    configureDecalAttribs();
  }
  else
  {
    glBindVertexArray(target.vao);
    glBindBuffer(GL_ARRAY_BUFFER, target.vbo);
  }

  // Draw geometry is one fixed quad per painted tile/face -- bounded, so a full
  // re-upload on the (infrequent) change is cheap.
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(count * sizeof(DecalVertex)),
               verts,
               GL_STATIC_DRAW);
  target.count = static_cast<int>(count);
  target.capacity = static_cast<int>(count);

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void IsometricGeometryRenderer::freePaintTarget(std::int64_t key)
{
  auto it = m_paintTargets.find(key);
  if (it == m_paintTargets.end())
    return;

  if (it->second.tex != 0)
    glDeleteTextures(1, &it->second.tex);
  if (it->second.vbo != 0)
    glDeleteBuffers(1, &it->second.vbo);
  if (it->second.vao != 0)
    glDeleteVertexArrays(1, &it->second.vao);

  m_paintTargets.erase(it);
}

void IsometricGeometryRenderer::drawPaintTarget(std::int64_t key)
{
  initialize();
  if (!initialized)
    return;

  PaintTarget* target = findPaintTarget(key);
  if (target == nullptr || target->tex == 0 || target->count == 0)
    return;

  beginDecalPipeline();
  // The paint texture stores premultiplied alpha; composite it over the scene
  // with premultiplied "over" (drawDecalsDynamic restores straight alpha).
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBindTexture(GL_TEXTURE_2D, target->tex);
  glBindVertexArray(target->vao);
  glDrawArrays(GL_TRIANGLES, 0, target->count);
  SFS_GL_CHECK("paintTarget");
  glBindVertexArray(0);
}

void IsometricGeometryRenderer::drawDecalsDynamic(const DecalVertex* vertices,
                                                  std::size_t count,
                                                  unsigned int texture)
{
  initialize();
  if (!initialized || texture == 0 || count == 0)
    return;

  beginDecalPipeline();
  // Dynamic decals carry straight-alpha colour; restore straight "over" in case
  // a baked paint target left premultiplied blend set.
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glBindVertexArray(decalDynamicVao);
  glBindBuffer(GL_ARRAY_BUFFER, decalDynamicVbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(count * sizeof(DecalVertex)),
               vertices,
               GL_DYNAMIC_DRAW);

  glBindTexture(GL_TEXTURE_2D, texture);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));
  SFS_GL_CHECK("decalsDynamic");

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void IsometricGeometryRenderer::setGeometryLighting(float ambient,
                                                    glm::vec3 lightColor,
                                                    glm::vec3 sunDirection,
                                                    float diffuseStrength)
{
  m_geomAmbient = ambient;
  m_geomLightColor = lightColor;
  m_geomSunDirection = sunDirection;
  m_geomDiffuseStrength = diffuseStrength;
}

void IsometricGeometryRenderer::beginGeometryPipeline()
{
  if (m_pipeline == Pipeline::Geometry)
    return;

  flushCurrentPipeline();
  m_pipeline = Pipeline::Geometry;

  glUseProgram(geometryShaderProgram);

  // Opaque terrain faces: depth test + WRITE, alpha-blended cutout edges.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Frame uniforms (locations fetched once per frame; cheap). Sun/ambient from
  // setGeometryLighting; point lights + heightmap from the renderer's frame
  // state.
  glUniform1i(SFS_GL_UNIFORM(geometryShaderProgram, "uTexture"), 0);
  glUniform1f(SFS_GL_UNIFORM(geometryShaderProgram, "uAmbient"), m_geomAmbient);
  glUniform3f(SFS_GL_UNIFORM(geometryShaderProgram, "uLightDirection"),
              m_geomSunDirection.x,
              m_geomSunDirection.y,
              m_geomSunDirection.z);
  glUniform3f(SFS_GL_UNIFORM(geometryShaderProgram, "uLightColor"),
              m_geomLightColor.x,
              m_geomLightColor.y,
              m_geomLightColor.z);
  glUniform1f(SFS_GL_UNIFORM(geometryShaderProgram, "uDiffuseStrength"),
              m_geomDiffuseStrength);
  glUniform1f(SFS_GL_UNIFORM(geometryShaderProgram, "uClipElevation"),
              m_geomClipElevation);

  glUniform1i(SFS_GL_UNIFORM(geometryShaderProgram, "uLightCount"),
              m_pointLights.count);
  if (m_pointLights.count > 0)
  {
    glUniform2fv(SFS_GL_UNIFORM(geometryShaderProgram, "uLightPositions[0]"),
                 m_pointLights.count,
                 &m_pointLights.positions[0].x);
    glUniform3fv(SFS_GL_UNIFORM(geometryShaderProgram, "uLightColors[0]"),
                 m_pointLights.count,
                 &m_pointLights.colors[0].x);
    glUniform1fv(SFS_GL_UNIFORM(geometryShaderProgram, "uLightIntensities[0]"),
                 m_pointLights.count,
                 m_pointLights.intensities);
    glUniform1fv(SFS_GL_UNIFORM(geometryShaderProgram, "uLightRadii[0]"),
                 m_pointLights.count,
                 m_pointLights.radii);
    glUniform1fv(SFS_GL_UNIFORM(geometryShaderProgram, "uLightHeights[0]"),
                 m_pointLights.count,
                 m_pointLights.heights);
    glUniform1fv(SFS_GL_UNIFORM(geometryShaderProgram, "uLightGroundLevels[0]"),
                 m_pointLights.count,
                 m_pointLights.groundLevels);
  }

  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, heightmapTextures[m_heightmapRing]);
  glUniform1i(SFS_GL_UNIFORM(geometryShaderProgram, "uHeightmap"), 2);
  glUniform2f(SFS_GL_UNIFORM(geometryShaderProgram, "uHeightmapOrigin"),
              static_cast<float>(m_heightmapOriginX),
              static_cast<float>(m_heightmapOriginY));
  glUniform2f(SFS_GL_UNIFORM(geometryShaderProgram, "uHeightmapSize"),
              static_cast<float>(m_heightmapWidth),
              static_cast<float>(m_heightmapHeight));
  glUniform1f(SFS_GL_UNIFORM(geometryShaderProgram, "uHeightmapTexSize"),
              static_cast<float>(m_heightmapTexSize));
  glUniform1f(
      SFS_GL_UNIFORM(geometryShaderProgram, "uHeightScale"), m_heightScale);

  glUniform1i(SFS_GL_UNIFORM(geometryShaderProgram, "uShadowSharp"),
              m_sunShadowStyle == SunShadowStyle::Sharp ? 1 : 0);
  glUniform1i(SFS_GL_UNIFORM(geometryShaderProgram, "uSunShadowEnabled"),
              m_sunShadowMarchEnabled ? 1 : 0);

  glActiveTexture(GL_TEXTURE0);
}

void IsometricGeometryRenderer::drawGeometry(const GeometryVertex* vertices,
                                             std::size_t count,
                                             unsigned int texture,
                                             int surfaceEffect)
{
  initialize();
  if (!initialized || texture == 0 || count == 0)
    return;

  beginGeometryPipeline();

  glUniform1i(
      SFS_GL_UNIFORM(geometryShaderProgram, "uSurfaceEffect"), surfaceEffect);

  // Convert screen-pixel positions to NDC (the rest of the vertex passes
  // through).
  m_geometryScratch.assign(vertices, vertices + count);
  for (auto& v : m_geometryScratch)
    v.position = toNdc(v.position);

  glBindVertexArray(geometryVao);
  glBindBuffer(GL_ARRAY_BUFFER, geometryVbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(count * sizeof(GeometryVertex)),
               m_geometryScratch.data(),
               GL_DYNAMIC_DRAW);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));
  SFS_GL_CHECK("drawGeometry");

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void IsometricGeometryRenderer::flushCurrentPipeline()
{
  ZoneScopedN("GL: pipeline flush");

  switch (m_pipeline)
  {
  case Pipeline::TerrainShadow:
  {
    TracyGpuZone("GPU: terrain shadow");
    flushTerrainShadow();
    m_pipeline = Pipeline::None;
    break;
  }

  case Pipeline::SpriteShadow:
    flushSpriteShadow();
    m_pipeline = Pipeline::None;
    break;

  // Core pipelines (solid, lit, particle) and the immediate-draw pipelines
  // (decals, geometry, textured, free-form) are handled by the base flush.
  default:
    OpenGLQuadRenderer::flushCurrentPipeline();
    break;
  }
}

std::string IsometricGeometryRenderer::litShaderImpl() const
{
  return std::string(sfs::shaders::quadIsoFrag);
}

unsigned int IsometricGeometryRenderer::createSurfaceShaderProgram() const
{
#ifdef __EMSCRIPTEN__
  const std::string glslVersion = "#version 300 es\n"
                                  "precision mediump float;\n";
#else
  const std::string glslVersion = "#version 330 core\n";
#endif

  const std::string vertexSource =
      glslVersion + std::string(sfs::shaders::surfaceVert);

  const std::string fragmentSource =
      glslVersion + std::string(sfs::shaders::surfaceFrag);

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
    LOG_ERROR(infoLog);

    glDeleteProgram(program);
    return 0;
  }

  return program;
}

unsigned int IsometricGeometryRenderer::createSpriteShadowShaderProgram() const
{
#ifdef __EMSCRIPTEN__
  const std::string glslVersion = "#version 300 es\n"
                                  "precision mediump float;\n";
#else
  const std::string glslVersion = "#version 330 core\n";
#endif

  const std::string vertexSource =
      glslVersion + std::string(sfs::shaders::spriteShadowVert);

  const std::string fragmentSource =
      glslVersion + std::string(sfs::shaders::spriteShadowFrag);

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
    LOG_ERROR(infoLog);

    glDeleteProgram(program);
    return 0;
  }

  return program;
}

unsigned int IsometricGeometryRenderer::createDecalShaderProgram() const
{
#ifdef __EMSCRIPTEN__
  const std::string glslVersion = "#version 300 es\n"
                                  "precision mediump float;\n";
#else
  const std::string glslVersion = "#version 330 core\n";
#endif

  // World-space decal: the vertex shader reproduces gridToIsometric +
  // worldToScreen + toNdc, and derives clip-space z from the painter sort-key
  // using the frame's depth range (matching assignClipDepth's toClipZ). This is
  // what lets settled decals live unchanged in GPU buffers across camera moves.
  const std::string vertexSource =
      glslVersion + std::string(sfs::shaders::decalVert);

  const std::string fragmentSource =
      glslVersion + std::string(sfs::shaders::decalFrag);

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
    LOG_ERROR(infoLog);
    glDeleteProgram(program);
    return 0;
  }

  return program;
}

unsigned int IsometricGeometryRenderer::createDecalBakeShaderProgram() const
{
#ifdef __EMSCRIPTEN__
  const std::string glslVersion = "#version 300 es\n"
                                  "precision mediump float;\n";
#else
  const std::string glslVersion = "#version 330 core\n";
#endif

  // Flat splat stamp: maps the target-local [0,1] coordinate straight to NDC
  // and writes the sprite premultiplied, so the paint texture accumulates by
  // area.
  const std::string vertexSource =
      glslVersion + std::string(sfs::shaders::decalBakeVert);

  const std::string fragmentSource =
      glslVersion + std::string(sfs::shaders::decalBakeFrag);

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
    LOG_ERROR(infoLog);
    glDeleteProgram(program);
    return 0;
  }

  return program;
}

unsigned int IsometricGeometryRenderer::createGeometryShaderProgram() const
{
#ifdef __EMSCRIPTEN__
  const std::string glslVersion = "#version 300 es\n"
                                  "precision mediump float;\n";
#else
  const std::string glslVersion = "#version 330 core\n";
#endif

  // Vertices arrive already projected to screen pixels -> NDC (CPU side), plus
  // world XY + per-vertex elevation (ground) + a real world normal for
  // lighting.
  const std::string vertexSource =
      glslVersion + std::string(sfs::shaders::geometryVert);

  // Adapted from the lit terrain shader for REAL geometry: the surface normal
  // is the real per-vertex normal (so "up" is normal.z, not the billboard
  // screen-space normal.y hack), and the fragment's ground is the interpolated
  // per-vertex elevation -- so a side face lights from its base up.
  const std::string fragmentSource =
      glslVersion + std::string(sfs::shaders::geometryFrag);

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
    LOG_ERROR(infoLog);
    glDeleteProgram(program);
    return 0;
  }

  return program;
}

} // namespace sfs
