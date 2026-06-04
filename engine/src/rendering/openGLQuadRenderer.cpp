
#include "engine/rendering/openGLQuadRenderer.h"

#include "engine/logger/logger.h"
#include "engine/rendering/batchKeys/LitQuadBatchKey.h"
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

OpenGLQuadRenderer::OpenGLQuadRenderer(int windowWidth, int windowHeight)
    : windowWidth(windowWidth), windowHeight(windowHeight)
{
  m_ndcScaleX = windowWidth > 0 ? 2.0f / static_cast<float>(windowWidth) : 0.0f;
  m_ndcScaleY =
      windowHeight > 0 ? 2.0f / static_cast<float>(windowHeight) : 0.0f;
}

OpenGLQuadRenderer::~OpenGLQuadRenderer() { shutdown(); }

bool OpenGLQuadRenderer::initialize()
{
  if (initialized)
    return true;

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
    return false;
  }

  // Simple untextured solid-color shader.
  // Used by:
  // - terrain shadows
  // - debug fills
  solidShaderProgram = createSolidShaderProgram();

  if (solidShaderProgram == 0)
  {
    LOG_ERROR("Failed to create OpenGLQuadRenderer solid shader program");
    return false;
  }

  // Particle shader.
  // Used by:
  // - unlit particle billboards (texture * per-vertex colour), batched by
  //   texture + blend mode.
  particleShaderProgram = createParticleShaderProgram();

  if (particleShaderProgram == 0)
  {
    LOG_ERROR("Failed to create OpenGLQuadRenderer particle shader");
    return false;
  }

  uParticleTextureLocation = SFS_GL_UNIFORM(particleShaderProgram, "uTexture");

  // ===========================================================================
  // Main textured/lit shader uniform locations
  // ===========================================================================

  uTextureLocation = SFS_GL_UNIFORM(shaderProgram, "uTexture");
  uColorLocation = SFS_GL_UNIFORM(shaderProgram, "uColor");
  uUseLightingLocation = SFS_GL_UNIFORM(shaderProgram, "uUseLighting");
  uNormalTextureLocation = SFS_GL_UNIFORM(shaderProgram, "uNormalTexture");
  uHasNormalMapLocation = SFS_GL_UNIFORM(shaderProgram, "uHasNormalMap");
  uLightDirectionLocation = SFS_GL_UNIFORM(shaderProgram, "uLightDirection");
  uLightColorLocation = SFS_GL_UNIFORM(shaderProgram, "uLightColor");
  uAmbientLocation = SFS_GL_UNIFORM(shaderProgram, "uAmbient");
  uDiffuseStrengthLocation = SFS_GL_UNIFORM(shaderProgram, "uDiffuseStrength");
  uSunShadowEnabledLocation =
      SFS_GL_UNIFORM(shaderProgram, "uSunShadowEnabled");
  uLightCountLocation = SFS_GL_UNIFORM(shaderProgram, "uLightCount");
  uLightPositionsLocation = SFS_GL_UNIFORM(shaderProgram, "uLightPositions[0]");
  uLightColorsLocation = SFS_GL_UNIFORM(shaderProgram, "uLightColors[0]");
  uLightIntensitiesLocation =
      SFS_GL_UNIFORM(shaderProgram, "uLightIntensities[0]");
  uLightRadiiLocation = SFS_GL_UNIFORM(shaderProgram, "uLightRadii[0]");
  uLightHeightsLocation = SFS_GL_UNIFORM(shaderProgram, "uLightHeights[0]");
  uLightGroundLevelsLocation =
      SFS_GL_UNIFORM(shaderProgram, "uLightGroundLevels[0]");
  uSurfaceEffectLocation = SFS_GL_UNIFORM(shaderProgram, "uSurfaceEffect");
  uHeightmapLocation = SFS_GL_UNIFORM(shaderProgram, "uHeightmap");
  uHeightmapOriginLocation = SFS_GL_UNIFORM(shaderProgram, "uHeightmapOrigin");
  uHeightmapSizeLocation = SFS_GL_UNIFORM(shaderProgram, "uHeightmapSize");
  uHeightmapTexSizeLocation =
      SFS_GL_UNIFORM(shaderProgram, "uHeightmapTexSize");
  uHeightScaleLocation = SFS_GL_UNIFORM(shaderProgram, "uHeightScale");

  glGenTextures(kHeightmapRingSize, heightmapTextures);

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
  // Particle VAO/VBO
  // Textured billboard + per-vertex RGBA modulate colour, batched by texture +
  // blend mode.
  // ===========================================================================

  glGenVertexArrays(1, &particleVao);
  glGenBuffers(1, &particleVbo);

  glBindVertexArray(particleVao);
  glBindBuffer(GL_ARRAY_BUFFER, particleVbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(ParticleVertex) * 6),
               nullptr,
               GL_DYNAMIC_DRAW);

  // position
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(
      0,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(ParticleVertex),
      reinterpret_cast<void*>(offsetof(ParticleVertex, position)));

  // uv
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(ParticleVertex),
                        reinterpret_cast<void*>(offsetof(ParticleVertex, uv)));

  // color (modulate)
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(
      2,
      4,
      GL_FLOAT,
      GL_FALSE,
      sizeof(ParticleVertex),
      reinterpret_cast<void*>(offsetof(ParticleVertex, color)));

  // clip-space depth
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3,
                        1,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(ParticleVertex),
                        reinterpret_cast<void*>(offsetof(ParticleVertex, z)));

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
  glUniform3f(uLightColorLocation, 1.0f, 1.0f, 1.0f);

  // Terrain/sprite default ambient light.
  glUniform1f(uAmbientLocation, 0.18f);
  glUniform1f(uDiffuseStrengthLocation, 0.85f);
  glUniform4f(uColorLocation, 1.0f, 1.0f, 1.0f, 1.0f);
  glUniform1i(uSurfaceEffectLocation, 0);

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

  SFS_GL_CHECK("OpenGLQuadRenderer::initialize");

  initialized = true;
  return true;
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

  if (particleVbo != 0)
    glDeleteBuffers(1, &particleVbo);

  if (particleVao != 0)
    glDeleteVertexArrays(1, &particleVao);

  if (particleShaderProgram != 0)
    glDeleteProgram(particleShaderProgram);

  particleVbo = 0;
  particleVao = 0;
  particleShaderProgram = 0;
  m_particleBatches.clear();

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
  SFS_GL_CHECK("drawImmediate");
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

void OpenGLQuadRenderer::submitParticleBatch(const ParticleBatch& batch,
                                             unsigned int texture,
                                             BlendMode blend,
                                             bool depthTested)
{
  initialize();

  if (!initialized || texture == 0 || batch.quads.empty())
    return;

  if (m_pipeline != Pipeline::Particle)
  {
    flushCurrentPipeline();
    m_pipeline = Pipeline::Particle;
  }

  appendParticleVertices(batch, texture, blend, depthTested);
}

void OpenGLQuadRenderer::appendParticleVertices(const ParticleBatch& batch,
                                                unsigned int texture,
                                                BlendMode blend,
                                                bool depthTested)
{
  std::vector<ParticleVertex>& verts =
      m_particleBatches[{texture, blend, depthTested}];
  verts.reserve(verts.size() + batch.quads.size() * 6);

  for (const auto& quad : batch.quads)
  {
    const glm::vec2 p0 = toNdc(quad.points[0]);
    const glm::vec2 p1 = toNdc(quad.points[1]);
    const glm::vec2 p2 = toNdc(quad.points[2]);
    const glm::vec2 p3 = toNdc(quad.points[3]);

    const float z = quad.z;

    verts.push_back({p0, quad.uvs[0], quad.color, z});
    verts.push_back({p1, quad.uvs[1], quad.color, z});
    verts.push_back({p2, quad.uvs[2], quad.color, z});

    verts.push_back({p0, quad.uvs[0], quad.color, z});
    verts.push_back({p2, quad.uvs[2], quad.color, z});
    verts.push_back({p3, quad.uvs[3], quad.color, z});
  }
}

void OpenGLQuadRenderer::flushParticles()
{
  ZoneScopedN("GL: flushParticles");
  TracyGpuZone("GPU: particles");

  initialize();

  if (!initialized || m_particleBatches.empty())
    return;

  glUseProgram(particleShaderProgram);

  // Never depth-write, so particles blend among themselves rather than
  // z-fighting. World particles depth-test against the opaque scene (terrain
  // occludes them); screen-space overlays skip the test (handled per bucket).
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);

  glActiveTexture(GL_TEXTURE0);
  glUniform1i(uParticleTextureLocation, 0);

  glBindVertexArray(particleVao);
  glBindBuffer(GL_ARRAY_BUFFER, particleVbo);

  // One draw per (texture, blend, depthTested) bucket.
  for (auto& [key, verts] : m_particleBatches)
  {
    if (verts.empty())
      continue;

    const auto& [texture, blend, depthTested] = key;

    if (depthTested)
    {
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LEQUAL);
    }
    else
    {
      glDisable(GL_DEPTH_TEST);
    }

    if (blend == BlendMode::Additive)
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindTexture(GL_TEXTURE_2D, texture);

    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(ParticleVertex)),
                 verts.data(),
                 GL_DYNAMIC_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
    SFS_GL_CHECK("particleBatch");
  }

  // Restore default state for whatever draws next.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glUseProgram(0);

  m_particleBatches.clear();
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
  SFS_GL_CHECK("solidFlush");

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
  glUniform3f(uLightColorLocation,
              key.lightColor.x,
              key.lightColor.y,
              key.lightColor.z);

  glUniform1f(uAmbientLocation, key.ambient);
  glUniform1f(uDiffuseStrengthLocation, key.diffuseStrength);
  glUniform1i(uSunShadowEnabledLocation, m_sunShadowMarchEnabled ? 1 : 0);

  glUniform4f(uColorLocation, 1.0f, 1.0f, 1.0f, 1.0f);

  glUniform1i(uLightCountLocation, m_pointLights.count);

  glUniform1i(uSurfaceEffectLocation, key.surfaceEffect);

  if (m_pointLights.count > 0)
  {
    glUniform2fv(uLightPositionsLocation,
                 m_pointLights.count,
                 reinterpret_cast<const float*>(m_pointLights.positions));

    glUniform3fv(uLightColorsLocation,
                 m_pointLights.count,
                 reinterpret_cast<const float*>(m_pointLights.colors));

    glUniform1fv(uLightIntensitiesLocation,
                 m_pointLights.count,
                 m_pointLights.intensities);

    glUniform1fv(uLightRadiiLocation, m_pointLights.count, m_pointLights.radii);
    glUniform1fv(
        uLightHeightsLocation, m_pointLights.count, m_pointLights.heights);
    glUniform1fv(uLightGroundLevelsLocation,
                 m_pointLights.count,
                 m_pointLights.groundLevels);
  }

  bindHeightmapUniforms();

  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(m_litVertices.size() * sizeof(Vertex)),
               m_litVertices.data(),
               GL_DYNAMIC_DRAW);

  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_litVertices.size()));
  SFS_GL_CHECK("litFlush");

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

  glDrawArrays(GL_LINE_STRIP, 0, count);
  SFS_GL_CHECK("drawLineLoop");

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
  glUniform3f(uLightColorLocation, lightColor.x, lightColor.y, lightColor.z);

  glUniform1f(uAmbientLocation, ambient);
  glUniform1f(uDiffuseStrengthLocation, diffuseStrength);
  glUniform1i(uSunShadowEnabledLocation, m_sunShadowMarchEnabled ? 1 : 0);

  glUniform4f(uColorLocation,
              tint.r / 255.0f,
              tint.g / 255.0f,
              tint.b / 255.0f,
              tint.a / 255.0f);
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

  bindHeightmapUniforms();

  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(vertices)),
               vertices,
               GL_DYNAMIC_DRAW);

  glDrawArrays(GL_TRIANGLES, 0, 6);
  SFS_GL_CHECK("debugQuad");
}

// Terrain heightmap for point-light occlusion (texture unit 2).
void OpenGLQuadRenderer::bindHeightmapUniforms()
{
  ZoneScopedN("GL: bind occlusion heightmap");

  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, heightmapTextures[m_heightmapRing]);
  glUniform1i(uHeightmapLocation, 2);
  glUniform2f(uHeightmapOriginLocation,
              static_cast<float>(m_heightmapOriginX),
              static_cast<float>(m_heightmapOriginY));
  glUniform2f(uHeightmapSizeLocation,
              static_cast<float>(m_heightmapWidth),
              static_cast<float>(m_heightmapHeight));
  glUniform1f(
      uHeightmapTexSizeLocation, static_cast<float>(m_heightmapTexSize));
  glUniform1f(uHeightScaleLocation, m_heightScale);
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
uniform vec3 uLightColor; // sun/ambient scene tint
uniform float uAmbient;
uniform float uDiffuseStrength;
uniform int uSunShadowEnabled; // 1 = cast terrain shadows via the heightmap march

uniform int uLightCount;
uniform vec2 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightRadii[MAX_LIGHTS];
uniform float uLightHeights[MAX_LIGHTS];
uniform float uLightGroundLevels[MAX_LIGHTS]; // terrain level under each light

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
// mountain taller than the lamp casts a shadow.
//
// Horizon-angle test: terrain blocks the light where its elevation ANGLE seen
// from the fragment's ground (height gained per tile of distance) rises above the
// angle to the light. Anchoring every occluder to the fragment this way means a
// flat ridge (angle 0) still beats a light sitting below the fragment (negative
// angle), so a fragment high on a plateau is correctly shadowed and the far side
// stays dark.
float pointLightVisibility(vec2 fragXY, vec2 lightXY, float fragGround,
                           float lightLevel)
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;

  // A fragment outside the grid has no valid ground; fall back to a flat plane at
  // the light's height there.
  float anchor = fragGround < -1.0e8 ? lightLevel : fragGround;

  vec2 toLight = lightXY - fragXY;
  float distTotal = length(toLight);

  if (distTotal < 1.0e-4)
    return 1.0;

  // Angle (levels per tile of horizontal distance) from the fragment's ground up
  // to the light. Negative when the light sits below the fragment.
  float lightAngle = (lightLevel - anchor) / distTotal;

  // Dense, evenly-spaced samples (no per-fragment jitter). Jitter dithers away
  // streaks for a static light, but for a moving light it turns the penumbra
  // into noise that shimmers along the shadow edge as the light moves. A finer
  // march removes the streaks without that flicker.
  const int STEPS = 48;

  float maxAngle = -1.0e9;

  for (int s = 0; s < STEPS; s++)
  {
    float t = (float(s) + 0.5) / float(STEPS);
    float d = distTotal * t;

    // Ignore terrain within ~one tile of the fragment so it can't self-occlude.
    // A smooth distance cutoff (rather than a per-tile test) keeps the shadow a
    // continuous function of position, with no seam as the fragment crosses a tile.
    if (d < 0.85)
      continue;

    vec2 samplePos = mix(fragXY, lightXY, t);
    float terrainAngle = (terrainLevelAt(samplePos) - anchor) / d;
    maxAngle = max(maxAngle, terrainAngle);
  }

  // How far (in angle) the highest occluder rises above the line to the light.
  // Feathering in ANGLE space keeps the penumbra a consistent softness at any
  // light distance (the level-equivalent width would shrink with range and read
  // as a hard edge far from the light). The wide band also smears the per-tile
  // anchor steps into a gradient, so the lit->dark transition is gradual.
  float over = maxAngle - lightAngle;
  return 1.0 - smoothstep(0.0, 0.30, over);
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

  // The fragment's ground feeds both the occlusion ray and the distance
  // attenuation. Sample it BILINEARLY so every term is a continuous function of
  // position -- the lighting then flows smoothly across the terrain instead of
  // snapping to the tile grid (a per-tile sample makes each elevation step a hard
  // brightness seam). Same for every light, so sample once.
  float fragGroundSmooth = terrainLevelAt(vWorldPosition);

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPosition;
    float distXY = length(delta);

    // Cheap horizontal reject (the full 3D distance only grows from here).
    if (distXY >= uLightRadii[i])
      continue;

    // Terrain between the fragment and the light blocks it, the same way a
    // mountain blocks the sun. The light sits at its emitter height (in levels)
    // above its ground. The ground level is supplied per light by the CPU (it
    // eases between tile elevations for a moving emitter), not sampled from the
    // heightmap here -- a heightmap sample would snap a whole level as the light
    // crossed a tile border and make the lit area pop.
    float lightLevel =
        uLightGroundLevels[i] + uLightHeights[i] * uHeightScale;

    // A point light is 3D: fold the elevation gap between the light and the
    // fragment's ground into the distance. Without this the light is a flat disc
    // that lights a whole mountain face at full strength just because its XY
    // distance is small -- it "climbs" and bleeds over tall terrain regardless of
    // height. LEVEL_TO_TILE is how many tiles of reach one elevation level costs;
    // higher = the light hugs the ground more and climbs tall terrain less.
    const float LEVEL_TO_TILE = 1.0;
    float fragGroundSafe =
        fragGroundSmooth < -1.0e8 ? lightLevel : fragGroundSmooth;
    float dz = (lightLevel - fragGroundSafe) * LEVEL_TO_TILE;
    float dist = sqrt(distXY * distXY + dz * dz);

    if (dist >= uLightRadii[i])
      continue;

    // The occlusion anchor is the bilinear ground, so the shadow is a continuous
    // function of position with no per-tile seam. The bilinear lift at a cliff
    // foot is harmless: the horizon-angle test still sees the adjacent cliff at a
    // steep angle and shadows it, and the wide penumbra hides the small residual.
    float visibility = pointLightVisibility(
        vWorldPosition, uLightPositions[i], fragGroundSmooth, lightLevel);

    if (visibility <= 0.001)
      continue;

    // Smootherstep falloff (3rd-order ease at both ends) so the pool fades into
    // darkness with no hard ring at the radius and no abrupt onset.
    float reach = clamp(1.0 - dist / uLightRadii[i], 0.0, 1.0);
    float attenuation = reach * reach * reach * (reach * (reach * 6.0 - 15.0) + 10.0);

    // The light vector's vertical leg is the elevation gap dz (in tiles), shared
    // with the 3D distance above, so the diffuse direction stays in world-tile
    // units and a taller emitter tilts the light more overhead.
    vec3 lightVector = vec3(delta.x, delta.y, max(dz, 0.0));
    vec3 pointDir = normalize(lightVector);

    float ndotl = max(dot(normal, pointDir), 0.0);
    float diffuse = mix(0.12, 1.0, pow(ndotl, 0.8));

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

// Directional sun occlusion against the terrain heightmap: 1 = lit, 0 = in a
// cast terrain shadow. Marches from the fragment along the sun azimuth and
// compares the terrain's max horizon angle to the sun's altitude -- the same
// horizon test the point lights use, fixed to the sun. Inert when no heightmap
// is uploaded (uHeightmapSize 0), so a flat-2D game casts no terrain shadows.
float sunVisibility()
{
  if (uHeightmapSize.x < 1.0)
    return 1.0;

  vec2 dir = uLightDirection.xy;
  float horiz = length(dir);
  if (horiz < 1.0e-4)
    return 1.0;

  dir /= horiz;
  float sunSlope = uLightDirection.z / horiz; // rise (levels) per tile

  float fragGround = terrainLevelAt(vWorldPosition);
  if (fragGround < -1.0e8)
    return 1.0; // outside the heightmap window

  const int STEPS = 24;
  const float kSunShadowMaxDist = 16.0; // tiles
  float maxAngle = -1.0e9;
  for (int s = 0; s < STEPS; s++)
  {
    float d = kSunShadowMaxDist * (float(s) + 1.0) / float(STEPS);
    if (d < 0.5)
      continue;
    float th = terrainLevelAt(vWorldPosition + dir * d);
    if (th < -1.0e8)
      continue;
    maxAngle = max(maxAngle, (th - fragGround) / d);
  }

  return 1.0 - smoothstep(0.0, 0.30, maxAngle - sunSlope);
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

  float lit = mix(litLevel, shadedLevel, shade);

  // Cast terrain shadows (heightmap horizon-march) pull the fragment toward its
  // shaded level, so a tile or sprite in another block's shadow loses direct sun
  // but keeps ambient. Disabled when the projected shadow technique is selected.
  if (uSunShadowEnabled == 1)
    lit = mix(shadedLevel, lit, sunVisibility());

  vec3 sunlight = vec3(lit) * uLightColor;

  sunlight = max(sunlight, vec3(0.03));

  // How much point lights assert themselves, driven purely by the ambient sky
  // level (uAmbient) -- NOT a day/night clock. Bright ambient drowns them out;
  // any time ambient drops they fade back in, so a storm darkening the sky at
  // midday lets point lights and their occlusion work normally.
  float daylight =
      smoothstep(0.20, 0.75, uAmbient);

  float pointVisibility =
      1.0 - daylight;

  pointVisibility =
      pointVisibility * pointVisibility;

  // Skip the point-light pass entirely when its contribution would round to
  // nothing. calculatePointLighting runs a 48-tap occlusion march per light per
  // pixel, so this reclaims that GPU cost whenever the sky is bright -- and since
  // the test is on uAmbient (uniform), the branch is coherent across the draw.
  vec3 pointLight = vec3(0.0);

  if (pointVisibility > 0.001)
    pointLight = calculatePointLighting(normal) * (pointVisibility * 2.0);

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
  m_particleBatches.clear();

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

unsigned int OpenGLQuadRenderer::createParticleShaderProgram() const
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
  // Unlit: the particle's colour fully modulates the sprite texture (RGB and
  // alpha). Premultiply-free; the blend mode decides how it combines.
  vec4 tex = texture(uTexture, vUv);
  FragColor = tex * vColor;

  if (FragColor.a <= 0.0)
    discard;
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

  case Pipeline::LitSprite:
    flushLit();
    break;

  case Pipeline::Particle:
    flushParticles();
    break;

  // The remaining pipelines either draw immediately or are handled by an
  // isometric subclass, so there's nothing queued to flush here.
  case Pipeline::TerrainShadow:
  case Pipeline::SpriteShadow:
  case Pipeline::Decal:
  case Pipeline::Geometry:
  case Pipeline::Textured:
  case Pipeline::Freeform:
  case Pipeline::None:
    break;
  }

  m_pipeline = Pipeline::None;
}

void OpenGLQuadRenderer::flush() { flushCurrentPipeline(); }

void OpenGLQuadRenderer::setSurfaceTime(float time) { m_surfaceTime = time; }

void OpenGLQuadRenderer::setPointLights(const PointLightSet& lights)
{
  m_pointLights = lights;
  m_pointLights.count = std::clamp(m_pointLights.count, 0, MaxShaderLights);
}

void OpenGLQuadRenderer::uploadHeightmap(const int* elevations,
                                         int width,
                                         int height,
                                         int originX,
                                         int originY,
                                         float heightScale)
{
  ZoneScopedN("GL: upload occlusion heightmap");
  TracyGpuZone("GPU: occlusion heightmap upload");

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

  // (Re)allocate the whole ring when the grid outgrows the current texture
  // (only the first call in practice -- the window is a fixed size). Every slot
  // is allocated at the same dimension so any of them can be bound for
  // sampling.
  if (texSize > m_heightmapTexSize)
  {
    const std::vector<float> empty(
        static_cast<size_t>(texSize) * texSize, -1000.0f);

    for (int i = 0; i < kHeightmapRingSize; i++)
    {
      glBindTexture(GL_TEXTURE_2D, heightmapTextures[i]);
      glTexImage2D(GL_TEXTURE_2D,
                   0,
                   GL_R32F,
                   texSize,
                   texSize,
                   0,
                   GL_RED,
                   GL_FLOAT,
                   empty.data());
      // Linear filtering ramps elevation smoothly between tiles so the
      // occlusion edge isn't tile-blocky.
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    m_heightmapTexSize = texSize;
  }

  // Advance to the next ring slot and upload into it. Writing a slot the GPU
  // finished sampling frames ago avoids the read-after-write stall that an
  // in-place update of the just-sampled texture would incur. Empty tiles get a
  // finite low value so they never occlude (the sentinel is INT_MIN).
  m_heightmapRing = (m_heightmapRing + 1) % kHeightmapRingSize;
  glBindTexture(GL_TEXTURE_2D, heightmapTextures[m_heightmapRing]);

  const size_t count = static_cast<size_t>(width) * height;
  m_heightmapScratch.resize(count);

  for (size_t i = 0; i < count; i++)
    m_heightmapScratch[i] =
        elevations[i] < -100000 ? -1000.0f : static_cast<float>(elevations[i]);

  glTexSubImage2D(GL_TEXTURE_2D,
                  0,
                  0,
                  0,
                  width,
                  height,
                  GL_RED,
                  GL_FLOAT,
                  m_heightmapScratch.data());

  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
}

} // namespace sfs
