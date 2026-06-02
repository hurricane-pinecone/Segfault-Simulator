
#include "engine/rendering/openGLQuadRenderer.h"

#include "engine/logger/logger.h"
#include "engine/rendering/batchKeys/LitQuadBatchKey.h"
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

OpenGLQuadRenderer::OpenGLQuadRenderer(int windowWidth, int windowHeight)
    : windowWidth(windowWidth), windowHeight(windowHeight)
{
  m_ndcScaleX = windowWidth > 0 ? 2.0f / static_cast<float>(windowWidth) : 0.0f;
  m_ndcScaleY =
      windowHeight > 0 ? 2.0f / static_cast<float>(windowHeight) : 0.0f;
}

OpenGLQuadRenderer::~OpenGLQuadRenderer() { shutdown(); }

void OpenGLQuadRenderer::initialize()
{
  if (initialized)
    return;

  // ===========================================================================
  // Create shader programs
  // ===========================================================================

  // Main textured + lit sprite shader.
  // Used by:
  // - terrain sprites
  // - actors
  // - lit quads
  // - textured debug rendering
  shaderProgram = createShaderProgram();

  if (shaderProgram == 0)
  {
    LOG_ERROR("Failed to create OpenGLQuadRenderer shader program");
    return;
  }

  // Simple untextured solid-color shader.
  // Used by:
  // - terrain shadows
  // - debug fills
  solidShaderProgram = createSolidShaderProgram();

  if (solidShaderProgram == 0)
  {
    LOG_ERROR("Failed to create OpenGLQuadRenderer solid shader program");
    return;
  }

  // Surface shader.
  // Used by:
  // - water rendering
  // - animated surface effects
  surfaceShaderProgram = createSurfaceShaderProgram();

  if (surfaceShaderProgram == 0)
  {
    LOG_ERROR("Failed to create OpenGLQuadRenderer surface shader program");
    return;
  }

  // Textured shadow shader.
  // Used by:
  // - projected sprite shadows (silhouette alpha from the sprite, per-vertex
  //   tint), batched by texture
  spriteShadowShaderProgram = createSpriteShadowShaderProgram();

  if (spriteShadowShaderProgram == 0)
  {
    LOG_ERROR("Failed to create OpenGLQuadRenderer sprite shadow shader");
    return;
  }

  uSpriteShadowTextureLocation =
      glGetUniformLocation(spriteShadowShaderProgram, "uTexture");

  // ===========================================================================
  // Main textured/lit shader uniform locations
  // ===========================================================================

  uTextureLocation = glGetUniformLocation(shaderProgram, "uTexture");
  uColorLocation = glGetUniformLocation(shaderProgram, "uColor");
  uUseLightingLocation = glGetUniformLocation(shaderProgram, "uUseLighting");
  uNormalTextureLocation =
      glGetUniformLocation(shaderProgram, "uNormalTexture");
  uHasNormalMapLocation = glGetUniformLocation(shaderProgram, "uHasNormalMap");
  uLightDirectionLocation =
      glGetUniformLocation(shaderProgram, "uLightDirection");
  uLightIntensityLocation =
      glGetUniformLocation(shaderProgram, "uLightIntensity");
  uAmbientLocation = glGetUniformLocation(shaderProgram, "uAmbient");
  uDiffuseStrengthLocation =
      glGetUniformLocation(shaderProgram, "uDiffuseStrength");
  uLightColorLocation = glGetUniformLocation(shaderProgram, "uLightColor");
  uLightCountLocation = glGetUniformLocation(shaderProgram, "uLightCount");
  uLightPositionsLocation =
      glGetUniformLocation(shaderProgram, "uLightPositions[0]");
  uLightColorsLocation = glGetUniformLocation(shaderProgram, "uLightColors[0]");
  uLightIntensitiesLocation =
      glGetUniformLocation(shaderProgram, "uLightIntensities[0]");
  uLightRadiiLocation = glGetUniformLocation(shaderProgram, "uLightRadii[0]");
  uLightHeightsLocation =
      glGetUniformLocation(shaderProgram, "uLightHeights[0]");
  uSurfaceEffectTimeLocation = glGetUniformLocation(shaderProgram, "uTime");
  uSurfaceEffectLocation =
      glGetUniformLocation(shaderProgram, "uSurfaceEffect");
  uHeightmapLocation = glGetUniformLocation(shaderProgram, "uHeightmap");
  uHeightmapOriginLocation =
      glGetUniformLocation(shaderProgram, "uHeightmapOrigin");
  uHeightmapSizeLocation =
      glGetUniformLocation(shaderProgram, "uHeightmapSize");
  uHeightmapTexSizeLocation =
      glGetUniformLocation(shaderProgram, "uHeightmapTexSize");
  uHeightScaleLocation = glGetUniformLocation(shaderProgram, "uHeightScale");

  glGenTextures(1, &heightmapTexture);

  // ===========================================================================
  // Surface shader uniform locations
  // ===========================================================================

  uSurfaceTimeLocation = glGetUniformLocation(surfaceShaderProgram, "uTime");
  uSurfaceRippleStrengthLocation =
      glGetUniformLocation(surfaceShaderProgram, "uRippleStrength");
  uSurfaceRippleScaleLocation =
      glGetUniformLocation(surfaceShaderProgram, "uRippleScale");
  uSurfaceAmbientLocation =
      glGetUniformLocation(surfaceShaderProgram, "uAmbient");
  uSurfaceLightCountLocation =
      glGetUniformLocation(surfaceShaderProgram, "uLightCount");
  uSurfaceLightPositionsLocation =
      glGetUniformLocation(surfaceShaderProgram, "uLightPositions[0]");
  uSurfaceLightColorsLocation =
      glGetUniformLocation(surfaceShaderProgram, "uLightColors[0]");
  uSurfaceLightIntensitiesLocation =
      glGetUniformLocation(surfaceShaderProgram, "uLightIntensities[0]");
  uSurfaceLightRadiiLocation =
      glGetUniformLocation(surfaceShaderProgram, "uLightRadii[0]");

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
  glVertexAttribPointer(
      5,
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
  // Main textured quad VAO/VBO
  // Used by sprites + lit textured rendering.
  // ===========================================================================

  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(Vertex) * 6),
               nullptr,
               GL_DYNAMIC_DRAW);

  // position
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, position)));

  // uv
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, uv)));

  // world position
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(
      2,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(Vertex),
      reinterpret_cast<void*>(offsetof(Vertex, worldPosition)));

  // clip-space depth
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3,
                        1,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, z)));

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // ===========================================================================
  // Solid color VAO/VBO
  // Used by terrain shadows and debug rendering.
  // ===========================================================================

  glGenVertexArrays(1, &solidVao);
  glGenBuffers(1, &solidVbo);

  glBindVertexArray(solidVao);
  glBindBuffer(GL_ARRAY_BUFFER, solidVbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(SolidVertex) * 6),
               nullptr,
               GL_DYNAMIC_DRAW);

  // position
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(SolidVertex),
      reinterpret_cast<void*>(offsetof(SolidVertex, position)));

  // color
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,
                        4,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(SolidVertex),
                        reinterpret_cast<void*>(offsetof(SolidVertex, color)));

  // clip-space depth
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2,
                        1,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(SolidVertex),
                        reinterpret_cast<void*>(offsetof(SolidVertex, z)));

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

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
  glVertexAttribPointer(
      2,
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
  // Default uniforms: textured/lit sprite shader
  // ===========================================================================

  glUseProgram(shaderProgram);

  glUniform1i(uTextureLocation, 0);
  glUniform1i(uNormalTextureLocation, 1);

  glUniform1i(uUseLightingLocation, 0);
  glUniform1i(uHasNormalMapLocation, 0);

  glUniform3f(uLightDirectionLocation, 0.0f, 0.0f, 1.0f);
  glUniform1f(uLightIntensityLocation, 1.0f);

  // Terrain/sprite default ambient light.
  glUniform1f(uAmbientLocation, 0.18f);
  glUniform1f(uDiffuseStrengthLocation, 0.85f);
  glUniform4f(uColorLocation, 1.0f, 1.0f, 1.0f, 1.0f);
  glUniform3f(uLightColorLocation, 1.0f, 1.0f, 1.0f);
  glUniform1i(uSurfaceEffectLocation, 0);

  glUseProgram(0);

  // ===========================================================================
  // Default uniforms: surface shader
  // ===========================================================================

  glUseProgram(surfaceShaderProgram);
  glUniform1f(uSurfaceTimeLocation, 0.0f);
  glUniform1f(uSurfaceRippleStrengthLocation, 0.025f);
  glUniform1f(uSurfaceRippleScaleLocation, 1.0f);

  // Water/surface default ambient.
  glUniform1f(uSurfaceAmbientLocation, 1.0f);

  glUseProgram(0);

  // ===========================================================================
  // Default normal map texture
  // ===========================================================================

  glGenTextures(1, &defaultNormalTexture);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, defaultNormalTexture);

  const unsigned char defaultNormalPixel[4] = {128, 128, 255, 255};

  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               1,
               1,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               defaultNormalPixel);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // ===========================================================================
  // White fallback texture
  // ===========================================================================

  glGenTextures(1, &whiteTexture);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, whiteTexture);

  const unsigned char whitePixel[4] = {255, 255, 255, 255};

  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               1,
               1,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               whitePixel);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glActiveTexture(GL_TEXTURE0);

  // ===========================================================================
  // Global OpenGL state
  // ===========================================================================

  // Opaque geometry tests and writes depth; per-pass depth state is (re)set in
  // begin() and the translucent flushes.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  initialized = true;
}

void OpenGLQuadRenderer::shutdown()
{
  if (!initialized)
    return;

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  for (auto& [id, texture] : textureCache)
  {
    if (texture != 0)
      glDeleteTextures(1, &texture);
  }

  textureCache.clear();

  if (vbo != 0)
    glDeleteBuffers(1, &vbo);

  if (vao != 0)
    glDeleteVertexArrays(1, &vao);

  if (shaderProgram != 0)
    glDeleteProgram(shaderProgram);

  if (whiteTexture != 0)
    glDeleteTextures(1, &whiteTexture);

  if (defaultNormalTexture != 0)
    glDeleteTextures(1, &defaultNormalTexture);

  if (solidVbo != 0)
    glDeleteBuffers(1, &solidVbo);

  if (solidVao != 0)
    glDeleteVertexArrays(1, &solidVao);

  if (solidShaderProgram != 0)
    glDeleteProgram(solidShaderProgram);

  if (spriteShadowVbo != 0)
    glDeleteBuffers(1, &spriteShadowVbo);

  if (spriteShadowVao != 0)
    glDeleteVertexArrays(1, &spriteShadowVao);

  if (spriteShadowShaderProgram != 0)
    glDeleteProgram(spriteShadowShaderProgram);

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

  solidVbo = 0;
  solidVao = 0;
  solidShaderProgram = 0;
  m_solidVertices.clear();

  whiteTexture = 0;
  defaultNormalTexture = 0;

  vbo = 0;
  vao = 0;
  shaderProgram = 0;

  uSurfaceEffectLocation = -1;
  uTextureLocation = -1;
  uColorLocation = -1;

  initialized = false;
}

void OpenGLQuadRenderer::submit(const Quad& command)
{
  initialize();

  if (!initialized)
    return;

  beginPipeline(Pipeline::SolidColor);
  appendSolidVertices(command);
}

void OpenGLQuadRenderer::submit(const TexturedQuad& command)
{
  flush();
  drawImmediate(command);
}

void OpenGLQuadRenderer::submit(const FreeformQuad& command)
{
  flush();
  drawQuad(command);
}

void OpenGLQuadRenderer::submit(const LitQuad& command)
{
  initialize();

  if (!initialized)
    return;

  if (command.texture == 0)
    return;

  if (command.destRect.w <= 0 || command.destRect.h <= 0)
    return;

  if (command.textureWidth <= 0 || command.textureHeight <= 0)
    return;

  const LitBatchKey key = LitBatchKey::from(command, defaultNormalTexture);

  if (m_pipeline != Pipeline::LitSprite || !m_litBatchKey ||
      *m_litBatchKey != key)
  {
    flushCurrentPipeline();
    m_pipeline = Pipeline::LitSprite;
    m_litBatchKey = key;
  }

  appendLitVertices(command);
}

void OpenGLQuadRenderer::submitLitBatch(const LitQuadBatch& batch,
                                        unsigned int texture,
                                        unsigned int normalTexture,
                                        bool hasNormalMap,
                                        int surfaceEffect)
{
  initialize();

  if (!initialized || texture == 0 || batch.quads.empty())
    return;

  // Every quad in the batch shares one material and the frame-global
  // directional lighting (see IsometricRenderSystem::render), so build the
  // batch key once instead of per quad.
  const LitQuad& first = batch.quads.front();

  LitBatchKey key;
  key.texture = texture;
  key.normalTexture =
      hasNormalMap && normalTexture != 0 ? normalTexture : defaultNormalTexture;
  key.hasNormalMap = hasNormalMap && normalTexture != 0;
  key.lightDirection = first.lightDirection;
  key.lightIntensity = first.lightIntensity;
  key.ambient = first.ambient;
  key.diffuseStrength = first.diffuseStrength;
  key.lightColor = first.lightColor;
  key.surfaceEffect = surfaceEffect;

  if (m_pipeline != Pipeline::LitSprite || !m_litBatchKey ||
      *m_litBatchKey != key)
  {
    flushCurrentPipeline();
    m_pipeline = Pipeline::LitSprite;
    m_litBatchKey = key;
  }

  m_litVertices.reserve(m_litVertices.size() + batch.quads.size() * 6);

  for (const LitQuad& quad : batch.quads)
  {
    if (quad.destRect.w <= 0 || quad.destRect.h <= 0)
      continue;

    if (quad.textureWidth <= 0 || quad.textureHeight <= 0)
      continue;

    appendLitVertices(quad);
  }
}

void OpenGLQuadRenderer::submit(const SurfaceCommand& command)
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
  glUniform1f(uSurfaceRippleScaleLocation, 1.0f);
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

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);
}

void OpenGLQuadRenderer::appendSolidVertices(const Quad& command)
{
  initialize();

  if (!initialized)
    return;

  const glm::vec4 color{
      command.tint.r / 255.0f,
      command.tint.g / 255.0f,
      command.tint.b / 255.0f,
      command.tint.a / 255.0f,
  };

  const glm::vec2 p0 = toNdc(command.points[0]);
  const glm::vec2 p1 = toNdc(command.points[1]);
  const glm::vec2 p2 = toNdc(command.points[2]);
  const glm::vec2 p3 = toNdc(command.points[3]);

  const float z = command.z;

  m_solidVertices.push_back({p0, color, z});
  m_solidVertices.push_back({p1, color, z});
  m_solidVertices.push_back({p2, color, z});

  m_solidVertices.push_back({p0, color, z});
  m_solidVertices.push_back({p2, color, z});
  m_solidVertices.push_back({p3, color, z});
}

void OpenGLQuadRenderer::drawImmediate(const TexturedQuad& command)
{
  initialize();

  if (!initialized)
    return;

  if (command.texture == 0)
    return;

  if (command.destRect.w <= 0 || command.destRect.h <= 0)
    return;

  if (command.textureWidth <= 0 || command.textureHeight <= 0)
    return;

  const float left = static_cast<float>(command.destRect.x);
  const float right =
      static_cast<float>(command.destRect.x + command.destRect.w);

  const float top = static_cast<float>(command.destRect.y);
  const float bottom =
      static_cast<float>(command.destRect.y + command.destRect.h);

  const glm::vec2 p0 = toNdc({left, top});
  const glm::vec2 p1 = toNdc({right, top});
  const glm::vec2 p2 = toNdc({right, bottom});
  const glm::vec2 p3 = toNdc({left, bottom});

  drawQuadInternal(command.texture,
                   command.srcRect,
                   command.textureWidth,
                   command.textureHeight,
                   p0,
                   p1,
                   p2,
                   p3,
                   command.tint);
}

void OpenGLQuadRenderer::drawQuad(const FreeformQuad& command)
{
  initialize();

  if (!initialized)
    return;

  if (command.texture == 0)
    return;

  if (command.textureWidth <= 0 || command.textureHeight <= 0)
    return;

  const glm::vec2 p0 = toNdc(command.points[0]);
  const glm::vec2 p1 = toNdc(command.points[1]);
  const glm::vec2 p2 = toNdc(command.points[2]);
  const glm::vec2 p3 = toNdc(command.points[3]);

  drawQuadInternalWithUvs(
      command.texture, p0, p1, p2, p3, command.uvs, command.tint, command.z);
}

void OpenGLQuadRenderer::drawQuadInternalWithUvs(unsigned int texture,
                                                 const glm::vec2& p0,
                                                 const glm::vec2& p1,
                                                 const glm::vec2& p2,
                                                 const glm::vec2& p3,
                                                 const glm::vec2 uvs[4],
                                                 SDL_Color tint,
                                                 float z)
{
  const glm::vec2 worldPoints[4] = {
      {0.0f, 0.0f},
      {0.0f, 0.0f},
      {0.0f, 0.0f},
      {0.0f, 0.0f},
  };

  const Vertex vertices[6] = {
      {p0, uvs[0], worldPoints[0], z},
      {p1, uvs[1], worldPoints[1], z},
      {p2, uvs[2], worldPoints[2], z},

      {p0, uvs[0], worldPoints[0], z},
      {p2, uvs[2], worldPoints[2], z},
      {p3, uvs[3], worldPoints[3], z},
  };

  // Sprite shadows are translucent: they test against the opaque depth (so a
  // block in front occludes the shadow) but must not write depth.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);

  glUseProgram(shaderProgram);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glUniform1i(uTextureLocation, 0);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, defaultNormalTexture);
  glUniform1i(uNormalTextureLocation, 1);

  glUniform1i(uUseLightingLocation, 0);
  glUniform1i(uHasNormalMapLocation, 0);

  glUniform4f(uColorLocation,
              tint.r / 255.0f,
              tint.g / 255.0f,
              tint.b / 255.0f,
              tint.a / 255.0f);

  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(vertices)),
               vertices,
               GL_DYNAMIC_DRAW);

  glDrawArrays(GL_TRIANGLES, 0, 6);
}

void OpenGLQuadRenderer::appendLitVertices(const LitQuad& command)
{
  const float left = static_cast<float>(command.destRect.x);
  const float right =
      static_cast<float>(command.destRect.x + command.destRect.w);
  const float top = static_cast<float>(command.destRect.y);
  const float bottom =
      static_cast<float>(command.destRect.y + command.destRect.h);

  const glm::vec2 p0 = toNdc({left, top});
  const glm::vec2 p1 = toNdc({right, top});
  const glm::vec2 p2 = toNdc({right, bottom});
  const glm::vec2 p3 = toNdc({left, bottom});

  const float u0 = static_cast<float>(command.srcRect.x) / command.textureWidth;
  const float u1 = static_cast<float>(command.srcRect.x + command.srcRect.w) /
                   command.textureWidth;

  const float v0 =
      static_cast<float>(command.srcRect.y) / command.textureHeight;
  const float v1 = static_cast<float>(command.srcRect.y + command.srcRect.h) /
                   command.textureHeight;

  const float z = command.z;

  m_litVertices.push_back({p0, {u0, v0}, command.worldPoints[0], z});
  m_litVertices.push_back({p1, {u1, v0}, command.worldPoints[1], z});
  m_litVertices.push_back({p2, {u1, v1}, command.worldPoints[2], z});

  m_litVertices.push_back({p0, {u0, v0}, command.worldPoints[0], z});
  m_litVertices.push_back({p2, {u1, v1}, command.worldPoints[2], z});
  m_litVertices.push_back({p3, {u0, v1}, command.worldPoints[3], z});
}

void OpenGLQuadRenderer::submitSpriteShadow(const FreeformQuad& command)
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

void OpenGLQuadRenderer::appendSpriteShadowVertices(const FreeformQuad& command)
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

void OpenGLQuadRenderer::flushSpriteShadow()
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
  }

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);

  m_spriteShadowBatches.clear();
}

void OpenGLQuadRenderer::flushSolid()
{
  ZoneScopedN("GL: flushSolid");
  TracyGpuZone("GPU: solid");

  initialize();

  if (!initialized)
    return;

  if (m_solidVertices.empty())
    return;

  glUseProgram(solidShaderProgram);

  // Solid/debug fills are translucent overlays: test depth, do not write it.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glBindVertexArray(solidVao);
  glBindBuffer(GL_ARRAY_BUFFER, solidVbo);

  glBufferData(
      GL_ARRAY_BUFFER,
      static_cast<GLsizeiptr>(m_solidVertices.size() * sizeof(SolidVertex)),
      m_solidVertices.data(),
      GL_DYNAMIC_DRAW);

  gTerrainShadowFlushes++;
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_solidVertices.size()));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);

  m_solidVertices.clear();
}

void OpenGLQuadRenderer::flushLit()
{
  ZoneScopedN("GL: flushLit");
  TracyGpuZone("GPU: lit");

  initialize();

  if (!initialized)
    return;

  if (m_litVertices.empty() || !m_litBatchKey)
    return;

  const LitBatchKey& key = *m_litBatchKey;

  // Lit geometry is opaque: test + write depth. Cutout (discard on alpha <= 0
  // in the fragment shader) keeps transparent sprite corners from writing
  // depth and punching holes in what's behind them.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);

  glUseProgram(shaderProgram);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, key.texture);
  glUniform1i(uTextureLocation, 0);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, key.normalTexture);
  glUniform1i(uNormalTextureLocation, 1);

  glUniform1i(uUseLightingLocation, 1);
  glUniform1i(uHasNormalMapLocation, key.hasNormalMap ? 1 : 0);

  glUniform3f(uLightDirectionLocation,
              key.lightDirection.x,
              key.lightDirection.y,
              key.lightDirection.z);

  glUniform1f(uLightIntensityLocation, key.lightIntensity);
  glUniform1f(uAmbientLocation, key.ambient);
  glUniform1f(uDiffuseStrengthLocation, key.diffuseStrength);

  glUniform4f(uColorLocation, 1.0f, 1.0f, 1.0f, 1.0f);

  glUniform3f(uLightColorLocation,
              key.lightColor.x,
              key.lightColor.y,
              key.lightColor.z);

  glUniform1i(uLightCountLocation, m_pointLights.count);

  glUniform1f(uSurfaceEffectTimeLocation, m_surfaceTime);
  glUniform1i(uSurfaceEffectLocation, key.surfaceEffect);

  if (m_pointLights.count > 0)
  {
    glUniform2fv(uLightPositionsLocation,
                 m_pointLights.count,
                 reinterpret_cast<const float*>(m_pointLights.positions));

    glUniform3fv(uLightColorsLocation,
                 m_pointLights.count,
                 reinterpret_cast<const float*>(m_pointLights.colors));

    glUniform1fv(
        uLightIntensitiesLocation, m_pointLights.count, m_pointLights.intensities);

    glUniform1fv(uLightRadiiLocation, m_pointLights.count, m_pointLights.radii);
    glUniform1fv(uLightHeightsLocation, m_pointLights.count, m_pointLights.heights);
  }

  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(m_litVertices.size() * sizeof(Vertex)),
               m_litVertices.data(),
               GL_DYNAMIC_DRAW);

  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_litVertices.size()));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);

  m_litVertices.clear();
  m_litBatchKey.reset();
}

void OpenGLQuadRenderer::drawLineLoop(const glm::vec2* points,
                                      int count,
                                      SDL_Color color)
{
  initialize();

  if (!initialized || !points || count <= 1)
    return;

  std::vector<Vertex> vertices;
  vertices.reserve(static_cast<std::size_t>(count));

  for (int i = 0; i < count; i++)
    vertices.push_back({toNdc(points[i]), {0.0f, 0.0f}, {0.0f, 0.0f}, 0.0f});

  // Debug line overlays draw on top, unaffected by the depth buffer.
  glDisable(GL_DEPTH_TEST);

  glUseProgram(shaderProgram);

  glUniform1i(uUseLightingLocation, 0);
  glUniform1i(uHasNormalMapLocation, 0);

  glUniform4f(uColorLocation,
              color.r / 255.0f,
              color.g / 255.0f,
              color.b / 255.0f,
              color.a / 255.0f);

  glActiveTexture(GL_TEXTURE0);

  // Do NOT bind texture 0 here, because the shader samples uTexture
  // and discards when alpha <= 0.
  glBindTexture(GL_TEXTURE_2D, whiteTexture);
  glUniform1i(uTextureLocation, 0);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
               vertices.data(),
               GL_DYNAMIC_DRAW);

  glLineWidth(3.0f);
  glDrawArrays(GL_LINE_STRIP, 0, count);
  glLineWidth(1.0f);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);
}

void OpenGLQuadRenderer::drawQuadInternal(unsigned int texture,
                                          const SDL_Rect& srcRect,
                                          int textureWidth,
                                          int textureHeight,
                                          const glm::vec2& p0,
                                          const glm::vec2& p1,
                                          const glm::vec2& p2,
                                          const glm::vec2& p3,
                                          SDL_Color tint)
{
  const glm::vec2 worldPoints[4] = {
      glm::vec2{0.0f, 0.0f},
      glm::vec2{0.0f, 0.0f},
      glm::vec2{0.0f, 0.0f},
      glm::vec2{0.0f, 0.0f},
  };

  const glm::vec2 lightPositions[MaxShaderLights] = {};
  const glm::vec3 lightColors[MaxShaderLights] = {};
  const float lightIntensities[MaxShaderLights] = {};
  const float lightRadii[MaxShaderLights] = {};
  const float lightHeights[MaxShaderLights] = {};

  drawQuadInternal(texture,
                   srcRect,
                   textureWidth,
                   textureHeight,
                   p0,
                   p1,
                   p2,
                   p3,
                   tint,
                   false,
                   false,
                   0,
                   glm::vec3{0.0f, 0.0f, 1.0f},
                   1.0f,
                   1.0f,
                   0.0f,
                   glm::vec3{1.0f, 1.0f, 1.0f},
                   worldPoints,
                   0,
                   lightPositions,
                   lightColors,
                   lightIntensities,
                   lightRadii,
                   lightHeights);
}

void OpenGLQuadRenderer::drawQuadInternal(
    unsigned int texture,
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
    const float lightHeights[MaxShaderLights])
{
  const float u0 = static_cast<float>(srcRect.x) / textureWidth;
  const float u1 = static_cast<float>(srcRect.x + srcRect.w) / textureWidth;

  const float v0 = static_cast<float>(srcRect.y) / textureHeight;
  const float v1 = static_cast<float>(srcRect.y + srcRect.h) / textureHeight;

  const Vertex vertices[6] = {
      {p0, {u0, v0}, worldPoints[0]},
      {p1, {u1, v0}, worldPoints[1]},
      {p2, {u1, v1}, worldPoints[2]},

      {p0, {u0, v0}, worldPoints[0]},
      {p2, {u1, v1}, worldPoints[2]},
      {p3, {u0, v1}, worldPoints[3]},
  };

  // Immediate UI/text draws on top of the world, unaffected by the depth
  // buffer.
  glDisable(GL_DEPTH_TEST);

  glUseProgram(shaderProgram);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glUniform1i(uTextureLocation, 0);

  glActiveTexture(GL_TEXTURE1);

  const bool actuallyUseNormalMap =
      useLighting && hasNormalMap && normalTexture != 0;

  glBindTexture(GL_TEXTURE_2D,
                actuallyUseNormalMap ? normalTexture : defaultNormalTexture);

  glUniform1i(uNormalTextureLocation, 1);

  glUniform1i(uUseLightingLocation, useLighting ? 1 : 0);
  glUniform1i(uHasNormalMapLocation, actuallyUseNormalMap ? 1 : 0);

  glUniform3f(uLightDirectionLocation,
              lightDirection.x,
              lightDirection.y,
              lightDirection.z);

  glUniform1f(uLightIntensityLocation, lightIntensity);
  glUniform1f(uAmbientLocation, ambient);
  glUniform1f(uDiffuseStrengthLocation, diffuseStrength);

  glUniform4f(uColorLocation,
              tint.r / 255.0f,
              tint.g / 255.0f,
              tint.b / 255.0f,
              tint.a / 255.0f);
  glUniform3f(uLightColorLocation, lightColor.x, lightColor.y, lightColor.z);
  const int clampedLightCount = std::clamp(lightCount, 0, MaxShaderLights);

  glUniform1i(uLightCountLocation, clampedLightCount);
  glUniform1i(uSurfaceEffectLocation, 0);

  if (clampedLightCount > 0)
  {
    glUniform2fv(uLightPositionsLocation,
                 clampedLightCount,
                 reinterpret_cast<const float*>(lightPositions));

    glUniform3fv(uLightColorsLocation,
                 clampedLightCount,
                 reinterpret_cast<const float*>(lightColors));

    glUniform1fv(
        uLightIntensitiesLocation, clampedLightCount, lightIntensities);

    glUniform1fv(uLightRadiiLocation, clampedLightCount, lightRadii);

    glUniform1fv(uLightHeightsLocation, clampedLightCount, lightHeights);
  }

  // Terrain heightmap for point-light occlusion (texture unit 2).
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, heightmapTexture);
  glUniform1i(uHeightmapLocation, 2);
  glUniform2f(uHeightmapOriginLocation,
              static_cast<float>(m_heightmapOriginX),
              static_cast<float>(m_heightmapOriginY));
  glUniform2f(uHeightmapSizeLocation,
              static_cast<float>(m_heightmapWidth),
              static_cast<float>(m_heightmapHeight));
  glUniform1f(uHeightmapTexSizeLocation,
              static_cast<float>(m_heightmapTexSize));
  glUniform1f(uHeightScaleLocation, m_heightScale);

  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(vertices)),
               vertices,
               GL_DYNAMIC_DRAW);

  glDrawArrays(GL_TRIANGLES, 0, 6);
}

void OpenGLQuadRenderer::setViewportSize(int width, int height)
{
  windowWidth = width;
  windowHeight = height;
  m_ndcScaleX = width > 0 ? 2.0f / static_cast<float>(width) : 0.0f;
  m_ndcScaleY = height > 0 ? 2.0f / static_cast<float>(height) : 0.0f;
}

glm::vec2 OpenGLQuadRenderer::toNdc(const glm::vec2& pixelPosition) const
{
  return {
      pixelPosition.x * m_ndcScaleX - 1.0f,
      1.0f - pixelPosition.y * m_ndcScaleY,
  };
}

unsigned int OpenGLQuadRenderer::compileShader(unsigned int type,
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
    LOG_ERROR(infoLog);

    glDeleteShader(shader);
    return 0;
  }

  return shader;
}

unsigned int OpenGLQuadRenderer::createShaderProgram() const
{
#ifdef __EMSCRIPTEN__
  const std::string glslVersion = "#version 300 es\n"
                                  "precision mediump float;\n";
#else
  const std::string glslVersion = "#version 330 core\n";
#endif

  const std::string vertexSource = glslVersion + R"(
layout (location = 0) in vec2 aPosition;
layout (location = 1) in vec2 aUv;
layout (location = 2) in vec2 aWorldPosition;
layout (location = 3) in float aZ;

out vec2 vUv;
out vec2 vWorldPosition;

void main()
{
  vUv = aUv;
  vWorldPosition = aWorldPosition;

  gl_Position = vec4(aPosition, aZ, 1.0);
}
)";

  const std::string fragmentSource = glslVersion + R"(
in vec2 vUv;
in vec2 vWorldPosition;

out vec4 FragColor;

#define MAX_LIGHTS 16

uniform sampler2D uTexture;
uniform sampler2D uNormalTexture;

uniform vec4 uColor;

uniform int uUseLighting;
uniform int uHasNormalMap;

uniform vec3 uLightDirection;
uniform float uAmbient;
uniform float uDiffuseStrength;

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];
uniform float uLightHeights[MAX_LIGHTS];

uniform float uTime;
uniform int uSurfaceEffect;

// Terrain heightmap: one texel per tile holding the tile elevation in levels.
uniform sampler2D uHeightmap;
uniform vec2 uHeightmapOrigin;    // min tile, world space
uniform vec2 uHeightmapSize;      // valid grid dimensions in tiles (0 = disabled)
uniform float uHeightmapTexSize;  // allocated texture dimension (>= grid)
uniform float uHeightScale;       // light emitter height -> elevation levels

// Terrain elevation (levels) at a world-space position, or a very low value
// outside the grid so it never occludes. The grid lives in the corner of a
// larger fixed texture, so normalize tile coords by the texture size.
float terrainLevelAt(vec2 world)
{
  if (uHeightmapSize.x < 1.0 || uHeightmapSize.y < 1.0)
    return -1e9;

  vec2 texel = world - uHeightmapOrigin; // grid tile coordinates

  if (texel.x < 0.0 || texel.x >= uHeightmapSize.x || texel.y < 0.0 ||
      texel.y >= uHeightmapSize.y)
    return -1e9;

  return texture(uHeightmap, texel / uHeightmapTexSize).r;
}

// How much of the light reaches the fragment, 0 (fully blocked) .. 1 (clear).
// Terrain taller than the lamp strictly between the two blocks it, i.e. a
// mountain taller than the lamp casts a shadow. The fragment's and the lamp's
// own tiles never occlude. The threshold is soft (a 1-level penumbra) and the
// heightmap samples bilinearly, so the shadow edge isn't tile-blocky.
float pointLightVisibility(vec2 fragXY, vec2 lightXY, float lightLevel)
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;

  // The fragment stands on its own tile, so it never self-occludes. The light's
  // own tile is handled by lightLevel (terrain there sits below the light), so it
  // is NOT skipped: skipping it makes a moving light pop occlusion on/off in one
  // frame as it crosses a peak. Letting lightLevel handle it keeps that smooth.
  ivec2 fragTile = ivec2(floor(fragXY));

  // Dense, evenly-spaced samples (no per-fragment jitter). Jitter dithers away
  // streaks for a static light, but for a moving light it turns the penumbra
  // into noise that shimmers along the shadow edge as the light moves. A finer
  // march removes the streaks without that flicker.
  const int STEPS = 48;

  float visibility = 1.0;

  for (int s = 0; s < STEPS; s++)
  {
    float t = (float(s) + 0.5) / float(STEPS);
    vec2 samplePos = mix(fragXY, lightXY, t);
    ivec2 sampleTile = ivec2(floor(samplePos));

    if (sampleTile == fragTile)
      continue;

    float over = terrainLevelAt(samplePos) - lightLevel;
    visibility = min(visibility, 1.0 - smoothstep(0.5, 1.5, over));
  }

  return visibility;
}

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

float isoTopMask(vec2 uv)
{
  vec2 p = uv - vec2(0.5, 0.25);

  float diamond =
      abs(p.x) / 0.5 +
      abs(p.y) / 0.25;

  return 1.0 - smoothstep(0.92, 1.02, diamond);
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

  float blade =
      smoothstep(0.72, 0.92, valueNoise(vec2(p.x * 0.7, p.y * 3.5)));

  color *= mix(vec3(1.0), vec3(0.75, 1.12, 0.65), blade * 0.35);

  return color;
}

vec3 applySandEffect(vec3 color, vec2 uv, vec2 worldPos)
{
  vec2 p =
      worldPos * 18.0 +
      uv * 72.0;

  // Rotate-ish coordinate basis for wind-shaped diagonal dunes.
  float along =
      p.x * 0.85 + p.y * 0.32;

  float across =
      p.x * -0.28 + p.y * 0.96;

  float warp =
      valueNoise(vec2(along * 0.08, across * 0.18)) * 2.0 - 1.0;

  float dune =
      sin(along * 0.42 + warp * 2.8) * 0.5 + 0.5;

  dune =
      smoothstep(0.38, 0.72, dune);

  float fineDune =
      sin(along * 1.4 + warp * 3.5) * 0.5 + 0.5;

  fineDune =
      smoothstep(0.48, 0.82, fineDune);

  float grain =
      valueNoise(p * 3.7);

  float specks =
      smoothstep(0.90, 0.985, valueNoise(p * 10.0));

  vec3 sand = color;

  // broad dune bands
  sand *= mix(0.90, 1.10, dune * 0.25);

  // smaller wind ripples
  sand *= mix(0.96, 1.06, fineDune * 0.15);

  // fine grain
  sand *= mix(0.97, 1.03, grain * 0.5);

  // sparse darker flecks
  sand -= specks * 0.025;

  return sand;
}

vec3 calculatePointLighting(vec3 normal)
{
  vec3 weightedColor = vec3(0.0);
  float totalWeight = 0.0;
  float strongestAmount = 0.0;

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPosition;
    float dist = length(delta);

    if (dist >= uLightRadii[i])
      continue;

    // Terrain between the fragment and the light blocks it, the same way a
    // mountain blocks the sun. The light sits at its tile's elevation plus its
    // emitter height (in levels). Sample the light's ground clamped into the
    // grid so a light near/just past the loaded edge (e.g. as terrain streams
    // while moving) never reads the out-of-bounds sentinel - that would flip the
    // whole light's occlusion for a frame and flash its shadowed area bright.
    vec2 lightGroundPos = clamp(uLightPositions[i],
                                uHeightmapOrigin + 0.5,
                                uHeightmapOrigin + uHeightmapSize - 0.5);
    float lightLevel =
        terrainLevelAt(lightGroundPos) + uLightHeights[i] * uHeightScale;

    float visibility =
        pointLightVisibility(vWorldPosition, uLightPositions[i], lightLevel);

    if (visibility <= 0.001)
      continue;

    float attenuation = 1.0 - dist / uLightRadii[i];
    attenuation = clamp(attenuation, 0.0, 1.0);
    attenuation = pow(attenuation, 1.35);

    vec3 lightVector = vec3(delta.x, delta.y, uLightHeights[i] * 0.65);
    vec3 pointDir = normalize(lightVector);

    float ndotl = max(dot(normal, pointDir), 0.0);
    float diffuse = mix(0.25, 1.0, pow(ndotl, 0.55));

    float amount =
        uLightIntensities[i] *
        attenuation *
        diffuse *
        visibility;

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

  float cappedAmount =
      strongestAmount / (1.0 + strongestAmount);

  cappedAmount *= 1.65;

  return blendedColor * cappedAmount;
}

void main()
{
  vec4 albedo = texture(uTexture, vUv);


float top = isoTopMask(vUv);

if (uSurfaceEffect == 3) // Grass
{
  vec3 grass = applyGrassEffect(albedo.rgb, vUv, vWorldPosition);
  albedo.rgb = mix(albedo.rgb, grass, top);
}
else if (uSurfaceEffect == 4) // Sand
{
  vec3 sand = applySandEffect(albedo.rgb, vUv, vWorldPosition);
  albedo.rgb = mix(albedo.rgb, sand, top);
}

  if (albedo.a <= 0.0)
    discard;

  if (uUseLighting == 0)
  {
    FragColor = albedo * uColor;
    return;
  }

  vec3 normal = vec3(0.0, 0.0, 1.0);
  float emissiveMask = 0.0;

  if (uHasNormalMap != 0)
  {
    vec3 normalSample = texture(uNormalTexture, vUv).rgb;

    emissiveMask = smoothstep(
        0.95,
        1.0,
        min(min(normalSample.r, normalSample.g), normalSample.b));

    normal = normalSample * 2.0 - 1.0;

    if (length(normal) < 0.001)
      normal = vec3(0.0, 0.0, 1.0);
    else
      normal = normalize(normal);
  }

  vec3 lightDir = normalize(uLightDirection);
  float sunDiffuse = max(dot(normal, lightDir), 0.0);

  // These normal maps are screen-space: every face shares roughly the same z, so
  // tops and sides are told apart by the green channel (normal.y = "up on
  // screen") — tops point up (~+0.8), side faces point down (~-0.4). Only a side
  // face that is ALSO turned away from the sun is shaded, so terrain tops stay
  // bright whatever the sun angle.
  float upness = smoothstep(0.0, 0.5, normal.y);
  float sunFacing = smoothstep(0.0, 0.6, sunDiffuse);
  float shade = (1.0 - upness) * (1.0 - sunFacing);

  // Top faces dim slightly as the sun drops toward the horizon: a higher sun
  // (lightDir.z) lets an up-facing surface catch more sky light. The top normal
  // barely faces the sun horizontally, so tie this to the sun's elevation rather
  // than the dot product. Kept subtle - noon tops read full, low-sun tops only
  // soften - and applied only to up-facing surfaces so side faces are untouched.
  float sunHeight = clamp(lightDir.z, 0.0, 1.0);
  float ambientLevel = uAmbient * mix(mix(0.8, 1.0, sunHeight), 1.0, 1.0 - upness);

  // Lit faces get ambient + direct sun. A fully shaded side drops to the same
  // brightness a cast terrain shadow leaves it at (direct sun removed), so a
  // sprite's dark side reads as dark as the shadows on the ground. 1.0 -
  // uDiffuseStrength mirrors the terrain shadow overlay (terrainShadowAlpha
  // defaults to 1); never brighter than the ambient sky term.
  float litLevel = ambientLevel + sunDiffuse * uDiffuseStrength;
  float shadedLevel = min(1.0 - uDiffuseStrength, uAmbient);

  vec3 sunlight = vec3(mix(litLevel, shadedLevel, shade));

  sunlight = max(sunlight, vec3(0.03));

  vec3 pointLight =
      calculatePointLighting(normal);

  float daylight =
      smoothstep(0.20, 0.75, uAmbient);

  float pointVisibility =
      1.0 - daylight;

  pointVisibility =
      pointVisibility * pointVisibility;

  pointLight *= pointVisibility * 2.0;

  float sunlightAmount =
      max(max(sunlight.r, sunlight.g), sunlight.b);

  // At night: point lights have full authority.
  // During day: point lights only affect shadowed areas.
  float shadowRoom =
      mix(
          1.0,
          1.0 - smoothstep(0.65, 1.0, sunlightAmount),
          daylight);

  vec3 totalLight =
      sunlight + pointLight * shadowRoom;

  totalLight =
      clamp(totalLight, 0.0, 1.0);

  vec3 litColor =
      albedo.rgb * totalLight;

  vec3 emissiveColor =
      albedo.rgb * 2.5;

  vec3 finalRgb =
      mix(litColor, emissiveColor, emissiveMask);

  FragColor =
      vec4(finalRgb, albedo.a) * uColor;
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

unsigned int OpenGLQuadRenderer::uploadSurfaceTexture(SDL_Surface* surface)
{
  initialize();

  if (!initialized)
    return 0;

  if (!surface)
    return 0;

  SDL_Surface* converted =
      SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);

  if (!converted)
    return 0;

  GLuint texture = 0;

  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               converted->w,
               converted->h,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               converted->pixels);

  SDL_FreeSurface(converted);

  return texture;
}

unsigned int
OpenGLQuadRenderer::getOrCreateTexture(const std::string& textureId,
                                       SDL_Surface* surface)
{
  initialize();

  if (!initialized)
    return 0;

  auto it = textureCache.find(textureId);

  if (it != textureCache.end())
    return it->second;

  const unsigned int texture = uploadSurfaceTexture(surface);

  if (texture == 0)
    return 0;

  textureCache.emplace(textureId, texture);

  return texture;
}

void OpenGLQuadRenderer::deleteTexture(unsigned int texture)
{
  if (texture != 0)
    glDeleteTextures(1, &texture);
}

void OpenGLQuadRenderer::begin()
{
  initialize();

  m_pipeline = Pipeline::None;

  m_solidVertices.clear();
  m_litVertices.clear();
  m_spriteShadowBatches.clear();

  m_litBatchKey.reset();

  // Opaque pass state: depth-test on, write on. Translucent passes (shadows,
  // water) turn the depth write off themselves before drawing. The frame's
  // depth buffer is cleared in Game::render().
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
}

unsigned int OpenGLQuadRenderer::createSolidShaderProgram() const
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
layout(location = 2) in float aZ;

out vec4 vColor;

void main()
{
  vColor = aColor;
  gl_Position = vec4(aPosition, aZ, 1.0);
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
    LOG_ERROR(infoLog);

    glDeleteProgram(program);
    return 0;
  }

  return program;
}

unsigned int OpenGLQuadRenderer::createSpriteShadowShaderProgram() const
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

void OpenGLQuadRenderer::beginPipeline(Pipeline pipeline)
{
  if (m_pipeline == pipeline)
    return;

  flushCurrentPipeline();
  m_pipeline = pipeline;
}

void OpenGLQuadRenderer::flushCurrentPipeline()
{
  ZoneScopedN("GL: pipeline flush");

  switch (m_pipeline)
  {
  case Pipeline::SolidColor:
    flushSolid();
    break;

  case Pipeline::TerrainShadow:
  {
    TracyGpuZone("GPU: terrain shadow");
    flushTerrainShadow();
    break;
  }

  case Pipeline::LitSprite:
    flushLit();
    break;

  case Pipeline::SpriteShadow:
    flushSpriteShadow();
    break;

  case Pipeline::Textured:
  case Pipeline::Freeform:
  case Pipeline::None:
    break;
  }

  m_pipeline = Pipeline::None;
}

void OpenGLQuadRenderer::flush() { flushCurrentPipeline(); }

unsigned int OpenGLQuadRenderer::createSurfaceShaderProgram() const
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
uniform float uRippleScale;
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

void OpenGLQuadRenderer::setSurfaceTime(float time) { m_surfaceTime = time; }

void OpenGLQuadRenderer::setPointLights(const PointLightSet& lights)
{
  m_pointLights = lights;
  m_pointLights.count = std::clamp(m_pointLights.count, 0, MaxShaderLights);
}

void OpenGLQuadRenderer::setHeightmap(const int* elevations,
                                      int width,
                                      int height,
                                      int originX,
                                      int originY,
                                      float heightScale)
{
  // An empty grid (e.g. a transient rebuild) would turn occlusion off and flash
  // the lights. Keep the previous heightmap instead.
  if (width <= 0 || height <= 0 || elevations == nullptr)
    return;

  m_heightmapWidth = width;
  m_heightmapHeight = height;
  m_heightmapOriginX = originX;
  m_heightmapOriginY = originY;
  m_heightScale = heightScale;

  const int texSize = std::max({128, width, height});

  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, heightmapTexture);

  // Keep one fixed-size texture and update it in place. The grid size oscillates
  // by a tile as the camera moves, so reallocating with glTexImage2D every frame
  // thrashes the texture and flickers the occlusion; glTexSubImage2D into a
  // larger fixed texture avoids that. Reallocation only happens if the grid ever
  // outgrows the current texture (rare).
  if (texSize > m_heightmapTexSize)
  {
    const std::vector<float> empty(static_cast<size_t>(texSize) * texSize,
                                   -1000.0f);

    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_R32F,
                 texSize,
                 texSize,
                 0,
                 GL_RED,
                 GL_FLOAT,
                 empty.data());
    // Linear filtering ramps elevation smoothly between tiles so the occlusion
    // edge isn't tile-blocky.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_heightmapTexSize = texSize;
  }

  // One float texel per tile holds the tile's elevation in levels. Empty tiles
  // get a finite low value so they never occlude and bilinear sampling near them
  // stays sane (the real empty sentinel is INT_MIN).
  std::vector<float> texels(static_cast<size_t>(width) * height);
  for (size_t i = 0; i < texels.size(); i++)
    texels[i] =
        elevations[i] < -100000 ? -1000.0f : static_cast<float>(elevations[i]);

  glTexSubImage2D(
      GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_FLOAT, texels.data());

  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
}

} // namespace sfs
