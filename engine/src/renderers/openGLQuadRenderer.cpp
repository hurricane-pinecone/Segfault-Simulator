
#include "engine/renderers/openGLQuadRenderer.h"

#include "engine/logger/logger.h"
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
}

OpenGLQuadRenderer::~OpenGLQuadRenderer() { shutdown(); }

void OpenGLQuadRenderer::initialize()
{
  if (initialized)
    return;

  shaderProgram = createShaderProgram();

  if (shaderProgram == 0)
  {
    LOG_ERROR("Failed to create OpenGLQuadRenderer shader program");
    return;
  }

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
  uTerrainShadowModeLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowMode");
  uTerrainShadowDirLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowDir");
  uTerrainShadowLengthLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowLength");
  uTerrainShadowElevationLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowElevation");
  uTerrainShadowCameraIsoLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowCameraIso");
  uTerrainShadowScreenCenterLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowScreenCenter");
  uTerrainShadowViewportSizeLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowViewportSize");
  uTerrainShadowZoomLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowZoom");
  uTerrainShadowTileSizeLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowTileSize");
  uTerrainShadowElevationStepLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowElevationStep");
  uTerrainShadowWorldScaleLocation =
      glGetUniformLocation(shaderProgram, "uTerrainShadowWorldScale");

  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(Vertex) * 6),
               nullptr,
               GL_DYNAMIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, position)));

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, uv)));

  glEnableVertexAttribArray(2);
  glVertexAttribPointer(
      2,
      2,
      GL_FLOAT,
      GL_FALSE,
      sizeof(Vertex),
      reinterpret_cast<void*>(offsetof(Vertex, worldPosition)));

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  glUseProgram(shaderProgram);

  glUniform1i(uNormalTextureLocation, 1);
  glUniform1i(uUseLightingLocation, 0);
  glUniform1i(uHasNormalMapLocation, 0);
  glUniform3f(uLightDirectionLocation, 0.0f, 0.0f, 1.0f);
  glUniform1f(uLightIntensityLocation, 1.0f);
  glUniform1f(uAmbientLocation, 0.18f);
  glUniform1f(uDiffuseStrengthLocation, 0.85f);
  glUniform1i(uTextureLocation, 0);
  glUniform4f(uColorLocation, 1.0f, 1.0f, 1.0f, 1.0f);
  glUniform3f(uLightColorLocation, 1.0f, 1.0f, 1.0f);
  glUniform1i(uTerrainShadowModeLocation, 0);

  glUseProgram(0);

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

  glActiveTexture(GL_TEXTURE0);

  glDisable(GL_DEPTH_TEST);
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

  vbo = 0;
  vao = 0;
  shaderProgram = 0;

  uTextureLocation = -1;
  uColorLocation = -1;

  initialized = false;
}

void OpenGLQuadRenderer::drawQuad(const QuadDrawCommand& command)
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

void OpenGLQuadRenderer::drawFreeformQuad(
    const FreeformQuadDrawCommand& command)
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

void OpenGLQuadRenderer::drawTerrainShadow(
    const TerrainShadowDrawCommand& command)
{
  initialize();

  if (!initialized)
    return;

  if (command.shadowLength <= 0.001f)
    return;

  struct TerrainVertex
  {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec2 worldPosition;
  };

  const TerrainVertex vertices[6] = {
      {command.edgeA, {0.0f, 0.0f}, command.edgeA},
      {command.edgeB, {0.0f, 0.0f}, command.edgeB},
      {command.edgeB, {1.0f, 0.0f}, command.edgeB},

      {command.edgeA, {0.0f, 0.0f}, command.edgeA},
      {command.edgeB, {1.0f, 0.0f}, command.edgeB},
      {command.edgeA, {1.0f, 0.0f}, command.edgeA},
  };

  glUseProgram(shaderProgram);

  glUniform1i(uTerrainShadowModeLocation, 1);

  glUniform2f(
      uTerrainShadowDirLocation, command.shadowDir.x, command.shadowDir.y);
  glUniform1f(uTerrainShadowLengthLocation, command.shadowLength);
  glUniform1f(
      uTerrainShadowElevationLocation, static_cast<float>(command.elevation));

  glUniform2f(uTerrainShadowCameraIsoLocation,
              command.isoCameraPosition.x,
              command.isoCameraPosition.y);

  glUniform2f(uTerrainShadowScreenCenterLocation,
              command.screenCenter.x,
              command.screenCenter.y);

  glUniform2f(uTerrainShadowViewportSizeLocation,
              static_cast<float>(windowWidth),
              static_cast<float>(windowHeight));

  glUniform1f(uTerrainShadowZoomLocation, command.zoom);

  glUniform2f(uTerrainShadowTileSizeLocation,
              static_cast<float>(command.tileWidth),
              static_cast<float>(command.tileHeight));

  glUniform1f(uTerrainShadowElevationStepLocation,
              static_cast<float>(command.elevationStep));

  glUniform1f(uTerrainShadowWorldScaleLocation, command.worldScale);

  glUniform4f(uColorLocation,
              command.tint.r / 255.0f,
              command.tint.g / 255.0f,
              command.tint.b / 255.0f,
              command.tint.a / 255.0f);

  glUniform1i(uUseLightingLocation, 0);
  glUniform1i(uHasNormalMapLocation, 0);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sizeof(vertices)),
               vertices,
               GL_DYNAMIC_DRAW);

  glDrawArrays(GL_TRIANGLES, 0, 6);

  glUniform1i(uTerrainShadowModeLocation, 0);
}

void OpenGLQuadRenderer::drawLitQuad(const LitQuadDrawCommand& command)
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
                   command.tint,
                   true,
                   command.hasNormalMap,
                   command.normalTexture,
                   command.lightDirection,
                   command.lightIntensity,
                   command.ambient,
                   command.diffuseStrength,
                   command.lightColor,
                   command.worldPoints,
                   command.lightCount,
                   command.lightPositions,
                   command.lightColors,
                   command.lightIntensities,
                   command.lightRadii,
                   command.lightHeights);
}

void OpenGLQuadRenderer::drawLineLoop(const glm::vec2* points,
                                      int count,
                                      SDL_Color color)
{
  initialize();

  if (!initialized)
    return;

  if (!points || count <= 1)
    return;

  std::vector<Vertex> vertices;
  vertices.reserve(static_cast<std::size_t>(count));

  for (int i = 0; i < count; i++)
    vertices.push_back({toNdc(points[i]), {0.0f, 0.0f}, {0.0f, 0.0f}});

  glUseProgram(shaderProgram);

  glUniform4f(uColorLocation,
              color.r / 255.0f,
              color.g / 255.0f,
              color.b / 255.0f,
              color.a / 255.0f);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
               vertices.data(),
               GL_DYNAMIC_DRAW);

  glDrawArrays(GL_LINE_STRIP, 0, count);
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
}

glm::vec2 OpenGLQuadRenderer::toNdc(const glm::vec2& pixelPosition) const
{
  return {
      pixelPosition.x / static_cast<float>(windowWidth) * 2.0f - 1.0f,
      1.0f - pixelPosition.y / static_cast<float>(windowHeight) * 2.0f,
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

out vec2 vUv;
out vec2 vWorldPosition;

uniform int uTerrainShadowMode;
uniform vec2 uTerrainShadowDir;
uniform float uTerrainShadowLength;
uniform float uTerrainShadowElevation;

uniform vec2 uTerrainShadowCameraIso;
uniform vec2 uTerrainShadowScreenCenter;
uniform vec2 uTerrainShadowViewportSize;
uniform float uTerrainShadowZoom;
uniform vec2 uTerrainShadowTileSize;
uniform float uTerrainShadowElevationStep;
uniform float uTerrainShadowWorldScale;

vec2 terrainGridToIso(vec2 gridPosition)
{
  return vec2(
    (gridPosition.x - gridPosition.y) *
      (uTerrainShadowTileSize.x * uTerrainShadowWorldScale) * 0.5,
    (gridPosition.x + gridPosition.y) *
      (uTerrainShadowTileSize.y * uTerrainShadowWorldScale) * 0.5
  );
}

vec2 terrainToNdc(vec2 pixelPosition)
{
  return vec2(
    pixelPosition.x / uTerrainShadowViewportSize.x * 2.0 - 1.0,
    1.0 - pixelPosition.y / uTerrainShadowViewportSize.y * 2.0
  );
}

void main()
{
  vUv = aUv;
  vWorldPosition = aWorldPosition;

  if (uTerrainShadowMode != 0)
  {
    // aPosition is used as world/grid position in terrain-shadow mode.
    // aUv.x is used as the extrude flag: 0 = edge, 1 = shadow tip.
    vec2 worldPosition =
        aPosition + uTerrainShadowDir * uTerrainShadowLength * aUv.x;

    vec2 screenPosition =
        (terrainGridToIso(worldPosition) - uTerrainShadowCameraIso) *
          uTerrainShadowZoom +
        uTerrainShadowScreenCenter;

    screenPosition.y -=
        uTerrainShadowElevation *
        uTerrainShadowElevationStep *
        uTerrainShadowWorldScale *
        uTerrainShadowZoom;

    gl_Position = vec4(terrainToNdc(screenPosition), 0.0, 1.0);
    return;
  }

  gl_Position = vec4(aPosition, 0.0, 1.0);
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

vec3 calculateLampLighting(vec3 normal)
{
  vec3 accumulated = vec3(0.0);

  for (int i = 0; i < MAX_LIGHTS; i++)
  {
    if (i >= uLightCount)
      break;

    vec2 delta = uLightPositions[i] - vWorldPosition;
    float dist = length(delta);

    if (dist >= uLightRadii[i])
      continue;

    float attenuation = 1.0 - dist / uLightRadii[i];
    attenuation = clamp(attenuation, 0.0, 1.0);

    attenuation = attenuation * attenuation;

    vec3 lightVector = vec3(delta.x, delta.y, uLightHeights[i]);
    vec3 lightDir = normalize(lightVector);

    float ndotl = max(dot(normal, lightDir), 0.0);
    float diffuse = pow(ndotl, 0.75);

    float sunFade = 1.0 - clamp(uDiffuseStrength, 0.0, 1.0);
    float lampBoost = mix(0.35, 2.75, sunFade);

    float amount =
        diffuse * uLightIntensities[i] * attenuation * lampBoost;

    vec3 color = uLightColors[i];

    // Normalize brightness WITHOUT changing hue.
    float maxChannel = max(max(color.r, color.g), color.b);

    if (maxChannel > 0.001)
      color /= maxChannel;
    else
      color = vec3(1.0);

    // Strong color identity.
    color = mix(vec3(1.0), color, 0.92);

    accumulated += color * amount;
  }

  // Soft overall clamp instead of per-channel tonemap.
  float peak = max(max(accumulated.r, accumulated.g), accumulated.b);

  if (peak > 1.0)
    accumulated /= peak;

  return accumulated;
}
void main()
{
  vec4 albedo = texture(uTexture, vUv);

  if (albedo.a <= 0.0)
  {
    discard;
  }

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

  vec3 lampLighting = calculateLampLighting(normal);

  vec3 lightDir = normalize(uLightDirection);
  float directionalDiffuse = max(dot(normal, lightDir), 0.0);

  vec3 sunLighting =
      vec3(uAmbient) + directionalDiffuse * uDiffuseStrength;

  vec3 sunLitColor = albedo.rgb * clamp(sunLighting, 0.0, 1.0);

  // Use grayscale surface response for colored lamps so red/blue lights
  // still show on green/brown/blue sprites.
  float albedoLuma = dot(albedo.rgb, vec3(0.299, 0.587, 0.114));
  vec3 lampSurface = mix(albedo.rgb, vec3(albedoLuma), 0.25);

float sunAmount = clamp(uDiffuseStrength, 0.0, 1.0);

// Lamps are subtle in daylight, strong at night.
float visibleLampStrength = mix(1.25, 0.15, sunAmount);

vec3 lampLitColor = lampSurface * lampLighting * visibleLampStrength;

  vec3 litColor = clamp(sunLitColor + lampLitColor, 0.0, 1.0);
  vec3 emissiveColor = albedo.rgb * 2.5;

  vec3 finalRgb = mix(litColor, emissiveColor, emissiveMask);

  FragColor = vec4(finalRgb, albedo.a) * uColor;
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

} // namespace sfs
