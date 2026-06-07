
#include "engine/runtime/rendering/gl/openGLQuadRenderer.h"

#include "engine/core/logger/logger.h"
#include "engine/core/rendering/batchKeys/LitQuadBatchKey.h"
#include "engine/core/rendering/quads.h"
#include "engine/core/util/profiling.h"
#include "engine/generated/embeddedShaders.h"
#include "engine/runtime/rendering/gl/glDebug.h"
#include "engine/runtime/rendering/gl/gpuProfiling.h"
#include "glm/glm/common.hpp"
#include "glm/glm/trigonometric.hpp"

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

  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4,
                        4,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, color)));

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

  // Every quad in the batch shares one material and the frame-global lighting
  // state, so build the batch key once instead of per quad.
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
                                                 Color tint,
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

  glm::vec2 c0{left, top};
  glm::vec2 c1{right, top};
  glm::vec2 c2{right, bottom};
  glm::vec2 c3{left, bottom};

  // Rotate the corners about the quad centre so a sprite can face an arbitrary
  // direction. Lighting samples (worldPoints) stay axis-aligned -- negligible
  // for the small sprites that use this.
  if (command.rotation != 0.0f)
  {
    const glm::vec2 center{(left + right) * 0.5f, (top + bottom) * 0.5f};
    const float s = glm::sin(command.rotation);
    const float c = glm::cos(command.rotation);
    const auto rotate = [&](glm::vec2 p)
    {
      const glm::vec2 d = p - center;
      return center + glm::vec2{d.x * c - d.y * s, d.x * s + d.y * c};
    };
    c0 = rotate(c0);
    c1 = rotate(c1);
    c2 = rotate(c2);
    c3 = rotate(c3);
  }

  const glm::vec2 p0 = toNdc(c0);
  const glm::vec2 p1 = toNdc(c1);
  const glm::vec2 p2 = toNdc(c2);
  const glm::vec2 p3 = toNdc(c3);

  const float u0 = static_cast<float>(command.srcRect.x) / command.textureWidth;
  const float u1 = static_cast<float>(command.srcRect.x + command.srcRect.w) /
                   command.textureWidth;

  const float v0 =
      static_cast<float>(command.srcRect.y) / command.textureHeight;
  const float v1 = static_cast<float>(command.srcRect.y + command.srcRect.h) /
                   command.textureHeight;

  // p0/p1 are the top (head) edge, p2/p3 the bottom (feet). A standing sprite
  // is a vertical surface, so its top sits nearer than its feet: zTop for the
  // top edge, z for the feet. zTop resolves equal to z for flat quads
  // (depthSpan 0).
  const float z = command.z;
  const float zTop = command.zTop;

  const glm::vec4 c{command.tint.r / 255.0f,
                    command.tint.g / 255.0f,
                    command.tint.b / 255.0f,
                    command.tint.a / 255.0f};

  m_litVertices.push_back({p0, {u0, v0}, command.worldPoints[0], zTop, c});
  m_litVertices.push_back({p1, {u1, v0}, command.worldPoints[1], zTop, c});
  m_litVertices.push_back({p2, {u1, v1}, command.worldPoints[2], z, c});

  m_litVertices.push_back({p0, {u0, v0}, command.worldPoints[0], zTop, c});
  m_litVertices.push_back({p2, {u1, v1}, command.worldPoints[2], z, c});
  m_litVertices.push_back({p3, {u0, v1}, command.worldPoints[3], z, c});
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
  // depth and punching holes in what's behind them. Actor sprites carry a depth
  // gradient over their height (see the render system) so the depth buffer
  // sorts them per-pixel against the real block faces.
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
                                          const Rect& srcRect,
                                          int textureWidth,
                                          int textureHeight,
                                          const glm::vec2& p0,
                                          const glm::vec2& p1,
                                          const glm::vec2& p2,
                                          const glm::vec2& p3,
                                          Color tint)
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
    const Rect& srcRect,
    int textureWidth,
    int textureHeight,
    const glm::vec2& p0,
    const glm::vec2& p1,
    const glm::vec2& p2,
    const glm::vec2& p3,
    Color tint,
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
  const int clampedLightCount = glm::clamp(lightCount, 0, MaxShaderLights);

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

  const std::string vertexSource =
      glslVersion + std::string(sfs::shaders::quadVert);

  const std::string fragmentSource =
      glslVersion + std::string(sfs::shaders::quadFrag);
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

  const std::string vertexSource =
      glslVersion + std::string(sfs::shaders::solidVert);

  const std::string fragmentSource =
      glslVersion + std::string(sfs::shaders::solidFrag);

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

  const std::string vertexSource =
      glslVersion + std::string(sfs::shaders::particleVert);

  const std::string fragmentSource =
      glslVersion + std::string(sfs::shaders::particleFrag);

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
  m_pointLights.count = glm::clamp(m_pointLights.count, 0, MaxShaderLights);
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

  const int texSize = glm::max(128, glm::max(width, height));

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
