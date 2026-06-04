
#include "engine/rendering/isometricGeometryRenderer.h"

#include "engine/logger/logger.h"
#include "engine/rendering/glDebug.h"
#include "engine/rendering/quads.h"
#include "engine/systems/isometric/isometricRenderSystem.h"
#include "engine/utils/gpuProfiling.h"
#include "engine/utils/profiling.h"
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

  for (auto& [key, buf] : m_decalChunks)
  {
    if (buf.vbo != 0)
      glDeleteBuffers(1, &buf.vbo);
    if (buf.vao != 0)
      glDeleteVertexArrays(1, &buf.vao);
  }
  m_decalChunks.clear();

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
  glUniform1f(uSurfaceRippleStrengthLocation, 0.025f);
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
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3,
                        4,
                        GL_FLOAT,
                        GL_FALSE,
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

  glActiveTexture(GL_TEXTURE0);
}

IsometricGeometryRenderer::DecalChunkBuffer&
IsometricGeometryRenderer::ensureDecalChunk(std::int64_t key)
{
  DecalChunkBuffer& buf = m_decalChunks[key];
  if (buf.vao == 0)
  {
    glGenVertexArrays(1, &buf.vao);
    glGenBuffers(1, &buf.vbo);
    glBindVertexArray(buf.vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf.vbo);
    configureDecalAttribs();
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    buf.count = 0;
    buf.capacity = 0;
  }
  return buf;
}

void IsometricGeometryRenderer::uploadDecalChunk(std::int64_t key,
                                                 const DecalVertex* vertices,
                                                 std::size_t count)
{
  initialize();
  if (!initialized)
    return;

  // Entering the decal pipeline first flushes any pending batch, so binding
  // buffers here can't corrupt another pipeline's VAO state.
  beginDecalPipeline();

  DecalChunkBuffer& buf = ensureDecalChunk(key);

  glBindVertexArray(buf.vao);
  glBindBuffer(GL_ARRAY_BUFFER, buf.vbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(count * sizeof(DecalVertex)),
               vertices,
               GL_STATIC_DRAW);
  buf.count = static_cast<int>(count);
  buf.capacity = static_cast<int>(count);

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void IsometricGeometryRenderer::appendDecalChunk(std::int64_t key,
                                                 const DecalVertex* vertices,
                                                 std::size_t count)
{
  initialize();
  if (!initialized || count == 0)
    return;

  beginDecalPipeline();

  DecalChunkBuffer& buf = ensureDecalChunk(key);
  const int needed = buf.count + static_cast<int>(count);

  if (needed > buf.capacity)
  {
    // Grow: allocate a larger VBO (doubling), copy the existing verts over on
    // the GPU (no CPU mirror), swap it in, and re-point the VAO at it.
    int newCapacity = buf.capacity > 0 ? buf.capacity * 2 : 64;
    if (newCapacity < needed)
      newCapacity = needed;

    unsigned int newVbo = 0;
    glGenBuffers(1, &newVbo);
    glBindBuffer(GL_ARRAY_BUFFER, newVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(newCapacity * sizeof(DecalVertex)),
                 nullptr,
                 GL_STATIC_DRAW);

    if (buf.count > 0)
    {
      glBindBuffer(GL_COPY_READ_BUFFER, buf.vbo);
      glBindBuffer(GL_COPY_WRITE_BUFFER, newVbo);
      glCopyBufferSubData(
          GL_COPY_READ_BUFFER,
          GL_COPY_WRITE_BUFFER,
          0,
          0,
          static_cast<GLsizeiptr>(buf.count * sizeof(DecalVertex)));
      glBindBuffer(GL_COPY_READ_BUFFER, 0);
      glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }

    glDeleteBuffers(1, &buf.vbo);
    buf.vbo = newVbo;
    buf.capacity = newCapacity;

    glBindVertexArray(buf.vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf.vbo);
    configureDecalAttribs(); // re-bind attribs to the new VBO
  }
  else
  {
    glBindVertexArray(buf.vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf.vbo);
  }

  glBufferSubData(GL_ARRAY_BUFFER,
                  static_cast<GLintptr>(buf.count * sizeof(DecalVertex)),
                  static_cast<GLsizeiptr>(count * sizeof(DecalVertex)),
                  vertices);
  buf.count = needed;

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void IsometricGeometryRenderer::freeDecalChunk(std::int64_t key)
{
  auto it = m_decalChunks.find(key);
  if (it == m_decalChunks.end())
    return;

  if (it->second.vbo != 0)
    glDeleteBuffers(1, &it->second.vbo);
  if (it->second.vao != 0)
    glDeleteVertexArrays(1, &it->second.vao);

  m_decalChunks.erase(it);
}

void IsometricGeometryRenderer::drawDecalChunk(std::int64_t key,
                                               unsigned int texture)
{
  initialize();
  if (!initialized || texture == 0)
    return;

  auto it = m_decalChunks.find(key);
  if (it == m_decalChunks.end() || it->second.count == 0)
    return;

  beginDecalPipeline();
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindVertexArray(it->second.vao);
  glDrawArrays(GL_TRIANGLES, 0, it->second.count);
  SFS_GL_CHECK("decalChunk");
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

unsigned int IsometricGeometryRenderer::createSurfaceShaderProgram() const
{
#ifdef __EMSCRIPTEN__
  const std::string glslVersion = "#version 300 es\n"
                                  "precision mediump float;\n";
#else
  const std::string glslVersion = "#version 330 core\n";
#endif

  const std::string vertexSource = glslVersion + R"(
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aWorldPosition;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUv;
layout(location = 4) in vec4 aParams;
layout(location = 5) in float aZ;

out vec2 vWorldPosition;
out vec4 vColor;
out vec2 vUv;
out vec4 vParams;

void main()
{
  vWorldPosition = aWorldPosition;
  vColor = aColor;
  vUv = aUv;
  vParams = aParams;

  gl_Position = vec4(aPosition, aZ, 1.0);
}
)";

  const std::string fragmentSource = glslVersion + R"(
#define MAX_LIGHTS 16

in vec2 vWorldPosition;
in vec4 vColor;
in vec2 vUv;
in vec4 vParams;

out vec4 FragColor;

uniform float uTime;
uniform float uRippleStrength;
uniform float uAmbient;

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];

float hash21(vec2 p)
{
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

vec2 hash22(vec2 p)
{
  return vec2(
      hash21(p),
      hash21(p + 19.19));
}

float causticCells(vec2 p)
{
  vec2 i = floor(p);
  vec2 f = fract(p);

  float d1 = 10.0;
  float d2 = 10.0;

  for (int y = -1; y <= 1; y++)
  {
    for (int x = -1; x <= 1; x++)
    {
      vec2 g = vec2(float(x), float(y));
      vec2 o = hash22(i + g);

      o = 0.5 + 0.5 * sin(uTime * 0.75 + 6.2831 * o);

      vec2 r = g + o - f;
      float d = dot(r, r);

      if (d < d1)
      {
        d2 = d1;
        d1 = d;
      }
      else if (d < d2)
      {
        d2 = d;
      }
    }
  }

  float edge = sqrt(d2) - sqrt(d1);

  return 1.0 - smoothstep(0.035, 0.105, edge);
}

void main()
{
  float r1 = sin(
      vWorldPosition.x * 1.25 +
      vWorldPosition.y * 0.55 +
      uTime * 2.2);

  float r2 = sin(
      vWorldPosition.x * -0.75 +
      vWorldPosition.y * 1.10 +
      uTime * 1.6);

  float ripple = (r1 + r2) * 0.5;

  vec3 color = vColor.rgb;
  color += ripple * uRippleStrength;

  vec3 pointLight = vec3(0.0);

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    float dist = length(uLightPositions[i] - vWorldPosition);

    if (dist >= uLightRadii[i])
      continue;

    float attenuation = 1.0 - dist / uLightRadii[i];
    attenuation = clamp(attenuation, 0.0, 1.0);
    attenuation = pow(attenuation, 1.35);

    pointLight +=
        uLightColors[i] *
        uLightIntensities[i] *
        attenuation *
        0.45;
  }

  float depth = vParams.x;

  vec2 p = vWorldPosition * 1.75;

  float c1 = causticCells(p + vec2(uTime * 0.08, uTime * 0.04));
  float c2 = causticCells(p * 1.7 - vec2(uTime * 0.05, uTime * 0.09));

  float caustic = max(c1, c2 * 0.25);
  caustic = pow(caustic, 2.4);

  float shallowFactor =
      1.0 - smoothstep(0.25, 3.0, depth);

  shallowFactor = pow(shallowFactor, 1.8);

  float pointLightAmount =
      max(max(pointLight.r, pointLight.g), pointLight.b);

  float sunCausticVisibility =
      smoothstep(0.18, 0.75, uAmbient);

  float pointCausticVisibility =
      smoothstep(0.02, 0.35, pointLightAmount);

  float causticVisibility =
      max(sunCausticVisibility, pointCausticVisibility);

  float causticStrength =
      0.25 * shallowFactor;

  causticStrength *=
      1.0 + clamp(pointLightAmount, 0.0, 1.0) * 2.5;

  vec3 causticColor = vec3(0.85, 0.97, 1.0);
  float ambientVisibility = mix(0.25, 1.0, clamp(uAmbient, 0.0, 1.0));

  float lightFloor = mix(0.06, 0.82, clamp(uAmbient, 0.0, 1.0));
  color *= max(uAmbient, lightFloor);

  // Keep transparent water readable at night.
  vec3 nightWaterFloor = vColor.rgb * 0.22;
  color = max(color, nightWaterFloor);

  color +=
      causticColor *
      caustic *
      causticStrength *
      causticVisibility;

  color += pointLight;
  color = clamp(color, 0.0, 1.0);

  FragColor = vec4(color, vColor.a);
})";

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

  const std::string vertexSource = glslVersion + R"(
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aZ;

out vec2 vUv;
out vec4 vColor;

void main()
{
  vUv = aUv;
  vColor = aColor;
  gl_Position = vec4(aPosition, aZ, 1.0);
}
)";

  const std::string fragmentSource = glslVersion + R"(
in vec2 vUv;
in vec4 vColor;

out vec4 FragColor;

uniform sampler2D uTexture;

void main()
{
  // Silhouette alpha comes from the sprite texture; the tint (typically black)
  // comes from the per-vertex color.
  float silhouette = texture(uTexture, vUv).a;

  if (silhouette <= 0.0)
    discard;

  FragColor = vec4(vColor.rgb, vColor.a * silhouette);
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
  const std::string vertexSource = glslVersion + R"(
layout(location = 0) in vec2 aWorldPos;
layout(location = 1) in float aElevation;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 4) in float aSortKey;

out vec2 vUv;
out vec4 vColor;
out vec2 vWorldPos;
out float vGround;

uniform vec2 uTileSize;       // (tileWidth, tileHeight)
uniform float uWorldScale;
uniform float uZoom;
uniform vec2 uCameraIso;
uniform vec2 uScreenCenter;
uniform float uElevationStep;
uniform vec2 uNdcScale;       // (2/width, 2/height)
uniform float uDepthMin;
uniform float uDepthInvRange;

void main()
{
  vUv = aUv;
  vColor = aColor;
  vWorldPos = aWorldPos;
  vGround = aElevation;

  vec2 iso = vec2(aWorldPos.x - aWorldPos.y, aWorldPos.x + aWorldPos.y)
             * uTileSize * uWorldScale * 0.5;
  vec2 screen = (iso - uCameraIso) * uZoom + uScreenCenter;
  screen.y -= aElevation * uElevationStep * uWorldScale * uZoom;

  vec2 ndc = vec2(screen.x * uNdcScale.x - 1.0, 1.0 - screen.y * uNdcScale.y);

  float t = clamp((aSortKey - uDepthMin) * uDepthInvRange, 0.0, 1.0);
  float clipZ = 0.9 - 1.8 * t;

  gl_Position = vec4(ndc, clipZ, 1.0);
}
)";

  const std::string fragmentSource = glslVersion + R"(
#define MAX_LIGHTS 16

in vec2 vUv;
in vec4 vColor;
in vec2 vWorldPos;
in float vGround;

out vec4 FragColor;

uniform sampler2D uTexture;

uniform float uAmbient;
uniform vec3 uAmbientColor;
uniform float uHeightScale;

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];
uniform float uLightHeights[MAX_LIGHTS];
uniform float uLightGroundLevels[MAX_LIGHTS];

// Point-light contribution, matching the lit shader's model (3D distance with
// the elevation gap, smootherstep falloff, colour-normalised blend) but without
// the terrain horizon occlusion -- a decal lies on a surface, and skipping the
// per-pixel march keeps it cheap. Normal is treated as up (ground-facing).
vec3 decalPointLight()
{
  vec3 weightedColor = vec3(0.0);
  float totalWeight = 0.0;
  float strongest = 0.0;

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPos;
    float distXY = length(delta);
    if (distXY >= uLightRadii[i])
      continue;

    float lightLevel = uLightGroundLevels[i] + uLightHeights[i] * uHeightScale;
    float dz = lightLevel - vGround;
    float dist = sqrt(distXY * distXY + dz * dz);
    if (dist >= uLightRadii[i])
      continue;

    float reach = clamp(1.0 - dist / uLightRadii[i], 0.0, 1.0);
    float attenuation =
        reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);

    vec3 pointDir = normalize(vec3(delta.x, delta.y, max(dz, 0.0)));
    float ndotl = max(pointDir.z, 0.0); // surface normal up
    float diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));

    float amount = uLightIntensities[i] * attenuation * diffuse;

    vec3 color = uLightColors[i];
    float mc = max(max(color.r, color.g), color.b);
    color = mc > 0.001 ? color / mc : vec3(1.0);

    weightedColor += color * amount;
    totalWeight += amount;
    strongest = max(strongest, amount);
  }

  if (totalWeight <= 0.001)
    return vec3(0.0);

  vec3 blended = weightedColor / totalWeight;
  float capped = (strongest / (1.0 + strongest)) * 1.65;
  return blended * capped;
}

void main()
{
  vec4 tex = texture(uTexture, vUv) * vColor;
  if (tex.a <= 0.0)
    discard;

  // Point lights only assert themselves as ambient drops (same gating as the lit
  // shader), so blood near a lamp lights up at night but day ambient dominates.
  float daylight = smoothstep(0.20, 0.75, uAmbient);
  float pointVisibility = 1.0 - daylight;
  pointVisibility *= pointVisibility;

  vec3 pointLight = vec3(0.0);
  if (pointVisibility > 0.001)
    pointLight = decalPointLight() * (pointVisibility * 2.0);

  vec3 lighting = uAmbientColor * uAmbient + pointLight;

  FragColor = vec4(tex.rgb * lighting, tex.a);
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
  const std::string vertexSource = glslVersion + R"(
layout(location = 0) in vec2 aPosition;   // NDC
layout(location = 1) in vec2 aWorldPos;
layout(location = 2) in float aGround;
layout(location = 3) in vec2 aUv;
layout(location = 4) in vec3 aNormal;
layout(location = 5) in float aZ;         // clip-space depth

out vec2 vUv;
out vec2 vWorldPos;
out float vGround;
out vec3 vNormal;

void main()
{
  vUv = aUv;
  vWorldPos = aWorldPos;
  vGround = aGround;
  vNormal = aNormal;
  gl_Position = vec4(aPosition, aZ, 1.0);
}
)";

  // Adapted from the lit terrain shader for REAL geometry: the surface normal
  // is the real per-vertex normal (so "up" is normal.z, not the billboard
  // screen-space normal.y hack), and the fragment's ground is the interpolated
  // per-vertex elevation -- so a side face lights from its base up.
  const std::string fragmentSource = glslVersion + R"(
#define MAX_LIGHTS 16

in vec2 vUv;
in vec2 vWorldPos;
in float vGround;
in vec3 vNormal;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform float uAmbient;
uniform vec3 uLightDirection;
uniform vec3 uLightColor; // sun/ambient scene tint
uniform float uDiffuseStrength;
uniform int uSurfaceEffect;
uniform int uShadowSharp;      // 0 = smooth (bilinear), 1 = sharp (per-tile DDA)
uniform int uSunShadowEnabled; // 1 = cast terrain shadows via the heightmap march

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];
uniform float uLightHeights[MAX_LIGHTS];
uniform float uLightGroundLevels[MAX_LIGHTS];

uniform sampler2D uHeightmap;
uniform vec2 uHeightmapOrigin;
uniform vec2 uHeightmapSize;
uniform float uHeightmapTexSize;
uniform float uHeightScale;

float hash21(vec2 p)
{
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

float valueNoise(vec2 p)
{
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = hash21(i);
  float b = hash21(i + vec2(1.0, 0.0));
  float c = hash21(i + vec2(0.0, 1.0));
  float d = hash21(i + vec2(1.0, 1.0));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec3 applyGrassEffect(vec3 color, vec2 uv, vec2 worldPos)
{
  vec2 p = worldPos * 12.0 + uv * 28.0;
  float n1 = valueNoise(p);
  float n2 = valueNoise(p * 2.7);
  float variation = n1 * 0.65 + n2 * 0.35;
  vec3 dark = vec3(0.55, 0.85, 0.45);
  vec3 light = vec3(1.15, 1.28, 0.82);
  color *= mix(dark, light, variation);
  float blade = smoothstep(0.72, 0.92, valueNoise(vec2(p.x * 0.7, p.y * 3.5)));
  color *= mix(vec3(1.0), vec3(0.75, 1.12, 0.65), blade * 0.35);
  return color;
}

vec3 applySandEffect(vec3 color, vec2 uv, vec2 worldPos)
{
  vec2 p = worldPos * 18.0 + uv * 72.0;
  float along = p.x * 0.85 + p.y * 0.32;
  float across = p.x * -0.28 + p.y * 0.96;
  float warp = valueNoise(vec2(along * 0.08, across * 0.18)) * 2.0 - 1.0;
  float dune = sin(along * 0.42 + warp * 2.8) * 0.5 + 0.5;
  dune = smoothstep(0.38, 0.72, dune);
  float fineDune = sin(along * 1.4 + warp * 3.5) * 0.5 + 0.5;
  fineDune = smoothstep(0.48, 0.82, fineDune);
  float grain = valueNoise(p * 3.7);
  float specks = smoothstep(0.90, 0.985, valueNoise(p * 10.0));
  vec3 sand = color;
  sand *= mix(0.90, 1.10, dune * 0.25);
  sand *= mix(0.96, 1.06, fineDune * 0.15);
  sand *= mix(0.97, 1.03, grain * 0.5);
  sand -= specks * 0.025;
  return sand;
}

float terrainLevelAt(vec2 world)
{
  if (uHeightmapSize.x < 1.0 || uHeightmapSize.y < 1.0)
    return -1e9;
  vec2 texel = world - uHeightmapOrigin;
  if (texel.x < 0.0 || texel.x >= uHeightmapSize.x || texel.y < 0.0 ||
      texel.y >= uHeightmapSize.y)
    return -1e9;
  return texture(uHeightmap, texel / uHeightmapTexSize).r;
}

float pointLightVisibility(vec2 fragXY, vec2 lightXY, float fragGround,
                           float lightLevel)
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;
  float anchor = fragGround < -1.0e8 ? lightLevel : fragGround;
  vec2 toLight = lightXY - fragXY;
  float distTotal = length(toLight);
  if (distTotal < 1.0e-4)
    return 1.0;
  float lightAngle = (lightLevel - anchor) / distTotal;
  const int STEPS = 48;
  float maxAngle = -1.0e9;
  for (int s = 0; s < STEPS; s++)
  {
    float t = (float(s) + 0.5) / float(STEPS);
    float d = distTotal * t;
    if (d < 0.85)
      continue;
    vec2 samplePos = mix(fragXY, lightXY, t);
    float terrainAngle = (terrainLevelAt(samplePos) - anchor) / d;
    maxAngle = max(maxAngle, terrainAngle);
  }
  float over = maxAngle - lightAngle;
  return 1.0 - smoothstep(0.0, 0.30, over);
}

// Real-normal point lighting. fragGround is the fragment's own elevation (vGround),
// so a side face's lower fragments are close to a low light and light up.
vec3 calculatePointLighting(vec3 normal)
{
  vec3 weightedColor = vec3(0.0);
  float totalWeight = 0.0;
  float strongestAmount = 0.0;

  float fragGroundSmooth = vGround;

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPos;
    float distXY = length(delta);
    if (distXY >= uLightRadii[i])
      continue;

    float lightLevel = uLightGroundLevels[i] + uLightHeights[i] * uHeightScale;
    float dz = (lightLevel - fragGroundSmooth);
    float dist = sqrt(distXY * distXY + dz * dz);
    if (dist >= uLightRadii[i])
      continue;

    float visibility =
        pointLightVisibility(vWorldPos, uLightPositions[i], fragGroundSmooth, lightLevel);
    if (visibility <= 0.001)
      continue;

    float reach = clamp(1.0 - dist / uLightRadii[i], 0.0, 1.0);
    float attenuation = reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);

    vec3 lightVector = vec3(delta.x, delta.y, max(dz, 0.0));
    vec3 pointDir = normalize(lightVector);
    float ndotl = max(dot(normal, pointDir), 0.0);
    float diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));

    float amount = uLightIntensities[i] * attenuation * diffuse * visibility;

    vec3 color = uLightColors[i];
    float maxChannel = max(max(color.r, color.g), color.b);
    if (maxChannel > 0.001)
      color /= maxChannel;
    else
      color = vec3(1.0);

    weightedColor += color * amount;
    totalWeight += amount;
    strongestAmount = max(strongestAmount, amount);
  }

  if (totalWeight <= 0.001)
    return vec3(0.0);

  vec3 blendedColor = weightedColor / totalWeight;
  float cappedAmount = (strongestAmount / (1.0 + strongestAmount)) * 1.65;
  return blendedColor * cappedAmount;
}

// Directional sun occlusion: the same horizon-angle test the point lights use,
// marched along the sun azimuth and compared to the sun's altitude. 1 = lit,
// 0 = shadowed. Analytic against the heightmap, so no bias/acne/peter-panning.
//
// Two user-selectable styles (uShadowSharp):
//  - Smooth: fixed-distance samples read the LINEAR-filtered heightmap, so the
//    occluder horizon ramps between tiles -> soft, rounded shadow edges.
//  - Sharp: a grid DDA visits every tile the ray crosses and reads its centre
//    (the exact discrete elevation) -> blocky, tile-aligned shadow edges.
float sunVisibility(float fragGround)
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;

  vec2 dir = uLightDirection.xy; // horizontal direction toward the sun
  float horiz = length(dir);
  if (horiz < 1.0e-4)
    return 1.0; // sun ~straight up: nothing casts

  dir /= horiz;
  float sunSlope = uLightDirection.z / horiz; // rise (levels) per tile

  float maxAngle = -1.0e9;

  if (uShadowSharp == 1)
  {
    // Grid DDA (Amanatides-Woo) from the fragment toward the sun: visit EVERY
    // tile the ray crosses, in order, reading each tile's centre. Visiting every
    // crossed tile (vs fixed-distance samples) means no occluder is skipped, so a
    // block casts one coherent silhouette instead of scattered bits.
    ivec2 tile = ivec2(floor(vWorldPos));
    ivec2 stepDir = ivec2(sign(dir.x), sign(dir.y));
    vec2 invDir = 1.0 / max(abs(dir), vec2(1.0e-6));
    vec2 tMax;
    tMax.x = (dir.x >= 0.0 ? float(tile.x) + 1.0 - vWorldPos.x
                           : vWorldPos.x - float(tile.x)) *
             invDir.x;
    tMax.y = (dir.y >= 0.0 ? float(tile.y) + 1.0 - vWorldPos.y
                           : vWorldPos.y - float(tile.y)) *
             invDir.y;

    const int MAX_STEPS = 28;
    const float kSunShadowMaxDist = 18.0; // tiles
    for (int i = 0; i < MAX_STEPS; i++)
    {
      // Step to the next tile boundary (the fragment's own tile is passed first,
      // so it never self-occludes).
      if (tMax.x < tMax.y)
      {
        tile.x += stepDir.x;
        tMax.x += invDir.x;
      }
      else
      {
        tile.y += stepDir.y;
        tMax.y += invDir.y;
      }

      vec2 center = vec2(tile) + 0.5;
      float dist = length(center - vWorldPos);
      if (dist > kSunShadowMaxDist)
        break;

      float th = terrainLevelAt(center);
      if (th < -1.0e8)
        continue;

      maxAngle = max(maxAngle, (th - fragGround) / dist);
    }

    // Tight penumbra keeps the blocky silhouette crisp.
    return 1.0 - smoothstep(0.0, 0.06, maxAngle - sunSlope);
  }

  // Smooth: even point samples against the linearly-filtered heightmap.
  const int STEPS = 24;
  const float kSunShadowMaxDist = 16.0; // tiles
  for (int s = 0; s < STEPS; s++)
  {
    float d = kSunShadowMaxDist * (float(s) + 1.0) / float(STEPS);
    if (d < 0.5)
      continue;
    float th = terrainLevelAt(vWorldPos + dir * d);
    if (th < -1.0e8)
      continue;
    maxAngle = max(maxAngle, (th - fragGround) / d);
  }

  // Wider penumbra for soft, rounded edges.
  return 1.0 - smoothstep(0.0, 0.30, maxAngle - sunSlope);
}

void main()
{
  vec3 normal = normalize(vNormal);

  vec4 albedo = texture(uTexture, vUv);

  // Surface shimmer applies to top faces (real up-facing).
  if (normal.z > 0.5)
  {
    if (uSurfaceEffect == 3)
      albedo.rgb = applyGrassEffect(albedo.rgb, vUv, vWorldPos);
    else if (uSurfaceEffect == 4)
      albedo.rgb = applySandEffect(albedo.rgb, vUv, vWorldPos);
  }

  if (albedo.a <= 0.0)
    discard;

  vec3 lightDir = normalize(uLightDirection);
  float sunDiffuse = max(dot(normal, lightDir), 0.0);

  // Real geometry: "up-ness" is the world Z of the normal (top faces ~1, sides ~0).
  float upness = smoothstep(0.0, 0.5, normal.z);
  float sunFacing = smoothstep(0.0, 0.6, sunDiffuse);
  float shade = (1.0 - upness) * (1.0 - sunFacing);

  float sunHeight = clamp(lightDir.z, 0.0, 1.0);
  float ambientLevel = uAmbient * mix(mix(0.8, 1.0, sunHeight), 1.0, 1.0 - upness);

  float litLevel = ambientLevel + sunDiffuse * uDiffuseStrength;
  float shadedLevel = min(1.0 - uDiffuseStrength, uAmbient);
  float lit = mix(litLevel, shadedLevel, shade);

  // Cast sun shadows pull the fragment toward its shaded (ambient-only) level,
  // so a shadowed surface keeps ambient but loses the directional sun. Disabled
  // when the projected shadow technique is selected.
  if (uSunShadowEnabled == 1)
    lit = mix(shadedLevel, lit, sunVisibility(vGround));

  vec3 sunlight = vec3(lit) * uLightColor;
  sunlight = max(sunlight, vec3(0.03));

  float daylight = smoothstep(0.20, 0.75, uAmbient);
  float pointVisibility = 1.0 - daylight;
  pointVisibility *= pointVisibility;

  vec3 pointLight = vec3(0.0);
  if (pointVisibility > 0.001)
    pointLight = calculatePointLighting(normal) * (pointVisibility * 2.0);

  float sunlightAmount = max(max(sunlight.r, sunlight.g), sunlight.b);
  float shadowRoom =
      mix(1.0, 1.0 - smoothstep(0.65, 1.0, sunlightAmount), daylight);

  vec3 totalLight = clamp(sunlight + pointLight * shadowRoom, 0.0, 1.0);

  FragColor = vec4(albedo.rgb * totalLight, albedo.a);
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
    LOG_ERROR(infoLog);
    glDeleteProgram(program);
    return 0;
  }

  return program;
}

} // namespace sfs
