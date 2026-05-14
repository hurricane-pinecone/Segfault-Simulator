#include "engine/systems/isometricRenderSystem.h"

#include "engine/components/spriteComponent.h"
#include "engine/ecs/registry.h"
#include "engine/logger/logger.h"
#include "engine/systems/cameraSystem.h"
#include "engine/systems/isometricLightingSystem.h"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace sfs
{

ElevationComponent::ElevationComponent(int level) : level(level) {}

IsometricRenderSystem::IsometricRenderSystem(AssetStore& assetStore,
                                             int windowWidth,
                                             int windowHeight,
                                             int tileWidth,
                                             int tileHeight)
    : assetStore(assetStore), windowWidth(windowWidth),
      windowHeight(windowHeight), tileWidth(tileWidth), tileHeight(tileHeight)
{
  registerComponent<SpriteComponent>();
  registerComponent<TransformComponent>();
}

IsometricRenderSystem::~IsometricRenderSystem() { shutdownOpenGL(); }

void IsometricRenderSystem::render()
{
  initializeOpenGL();
  beginBatches();

  auto* lightingSystem = registry->hasSystem<IsometricLightingSystem>()
                             ? &registry->getSystem<IsometricLightingSystem>()
                             : nullptr;

  if (lightingSystem)
    lightingSystem->rebuildLights();

  const glm::vec2 cameraPosition = getCameraPosition();

  const glm::vec2 screenCenter{static_cast<float>(windowWidth) / 2.0f,
                               static_cast<float>(windowHeight) / 2.0f};

  const glm::vec2 isoCameraPosition = gridToIsometric(cameraPosition);

  for (const auto& entity : getEntities())
  {
    const auto& transform = entity.getComponent<TransformComponent>();
    const auto& spriteComponent = entity.getComponent<SpriteComponent>();

    const auto sprite = assetStore.getSprite(spriteComponent.spriteId);

    if (!sprite)
    {
      LOG_ERROR("Attempted to render NULL isometric sprite");
      continue;
    }

    SDL_Surface* surface = assetStore.getSurface(sprite->textureId);

    if (!surface)
    {
      LOG_ERROR("NULL surface included in OpenGL isometric render loop");
      continue;
    }

    if (getOrCreateTexture(sprite->textureId) == 0)
    {
      LOG_ERROR("Failed to upload OpenGL texture");
      continue;
    }

    const glm::vec2 isoPosition = gridToIsometric(transform.position);

    const glm::vec2 screenPosition =
        isoPosition - isoCameraPosition + screenCenter;

    const int width =
        static_cast<int>(sprite->srcRect.w * (transform.scale.x * worldScale));
    const int height =
        static_cast<int>(sprite->srcRect.h * (transform.scale.y * worldScale));

    const float anchorX = spriteComponent.anchor.x * static_cast<float>(width);
    const float anchorY = spriteComponent.anchor.y * static_cast<float>(height);

    const glm::vec2 groundSamplePosition =
        getGroundSamplePosition(entity, transform);

    const int elevationLevel =
        getRenderElevationLevel(entity, groundSamplePosition);

    const int elevationOffset = static_cast<int>(
        std::round(elevationLevel * elevationStep * worldScale));
    const float waveOffset = getWaveOffset(groundSamplePosition);

    const glm::vec2 surfacePosition{
        screenPosition.x, screenPosition.y - elevationOffset - waveOffset};

    SDL_Rect dest{static_cast<int>(std::round(surfacePosition.x - anchorX)),
                  static_cast<int>(std::round(surfacePosition.y - anchorY)),
                  width,
                  height};

    float sortKey = transform.position.x + transform.position.y;

    if (isTileEntity(entity))
    {
      sortKey += static_cast<float>(elevationLevel) * 0.01f;
    }
    else
    {
      sortKey += static_cast<float>(elevationLevel) * 0.01f;
      sortKey += 0.005f;
    }

    RenderItem item;
    item.textureId = sprite->textureId;
    item.srcRect = sprite->srcRect;
    item.dest = dest;
    item.textureWidth = surface->w;
    item.textureHeight = surface->h;
    item.sortKey = sortKey;
    item.tint = SDL_Color{255, 255, 255, 255};
    item.renderLayer = isTileEntity(entity) ? 0 : 2;

    glm::vec2 spriteWorldSample =
        isTileEntity(entity) ? transform.position + glm::vec2{0.5f, 0.5f}
                             : transform.position;

    if (lightingSystem)
    {
      IsometricLightingSample lightingSample{
          spriteWorldSample, static_cast<float>(elevationOffset)};

      const auto lighting = lightingSystem->computeLighting(lightingSample);

      item.lightDirection = lighting.direction;
      item.lightIntensity = lighting.intensity;
      item.ambient = lighting.ambient;
      item.diffuseStrength = lighting.diffuseStrength;
    }

    if (entity.hasComponent<NormalMapComponent>())
    {
      const auto& normalMap = entity.getComponent<NormalMapComponent>();
      const auto normalSprite = assetStore.getSprite(normalMap.spriteId);

      if (normalSprite)
      {
        SDL_Surface* normalSurface =
            assetStore.getSurface(normalSprite->textureId);

        if (normalSurface && getOrCreateTexture(normalSprite->textureId) != 0)
        {
          item.hasNormalMap = true;
          item.normalTextureId = normalSprite->textureId;
          item.normalSrcRect = normalSprite->srcRect;
          item.normalTextureWidth = normalSurface->w;
          item.normalTextureHeight = normalSurface->h;
        }
      }
    }

    if (lightingSystem && !isTileEntity(entity))
    {
      const auto& lights = lightingSystem->getLights();

      for (const auto& light : lights)
      {
        glm::vec2 delta = spriteWorldSample - light.worldPosition;
        float distance = glm::length(delta);

        if (distance > light.radius || distance < 0.001f)
          continue;

        glm::vec2 shadowWorldDir = delta / distance;

        float attenuation = 1.0f - distance / light.radius;
        attenuation = std::pow(std::clamp(attenuation, 0.0f, 1.0f), 0.75f);

        float casterHeight = static_cast<float>(item.dest.h) * 0.75f;

        float visualLightHeight =
            std::max(light.height * 0.0625f,
                     1.0f); // Exaggerate shadows because it looks sick. 32
                            // height looks like 2 to cast a low light

        float shadowLength =
            (casterHeight * distance / visualLightHeight) * 1.0f;

        glm::vec2 shadowOffset =
            worldDirToShadowOffset(shadowWorldDir, shadowLength);

        float alpha =
            0.42f * attenuation * std::clamp(light.intensity, 0.0f, 2.0f);

        submitShadow(item, shadowOffset, alpha);
      }

      glm::vec3 sunDir = lightingSystem->getLightDirection();

      if (lightingSystem->getDiffuseStrength() > 0.001f && sunDir.z > 0.05f)
      {
        glm::vec2 sunHorizontal{sunDir.x, sunDir.y};
        float horizontalLength = glm::length(sunHorizontal);

        if (horizontalLength > 0.001f)
        {
          glm::vec2 shadowWorldDir = -sunHorizontal / horizontalLength;

          glm::vec2 isoDir =
              gridToIsometric(shadowWorldDir) - gridToIsometric({0.0f, 0.0f});

          if (glm::length(isoDir) > 0.001f)
          {
            isoDir = glm::normalize(isoDir);

            float noonFade = glm::smoothstep(0.0f, 0.25f, horizontalLength);

            float shadowLength =
                static_cast<float>(item.dest.h) *
                std::clamp(horizontalLength / sunDir.z, 0.0f, 3.5f) * 0.35f *
                noonFade;

            float shadowAlpha = 0.22f * noonFade;

            if (shadowLength > 0.5f && shadowAlpha > 0.01f)
              submitShadow(item, isoDir * shadowLength, shadowAlpha);
          }
        }
      }
    }

    submitSprite(item);
  }

  flushBatches();

  glBindVertexArray(0);
  glUseProgram(0);
}

void IsometricRenderSystem::beginBatches() { renderItems.clear(); }

void IsometricRenderSystem::submitSprite(const RenderItem& item)
{
  renderItems.push_back(item);
}

void IsometricRenderSystem::submitShadow(const RenderItem& caster,
                                         const glm::vec2& shadowOffset,
                                         float alpha,
                                         float sortKeyBias)
{
  RenderItem shadow = caster;

  shadow.isShadow = true;
  shadow.hasNormalMap = false;
  shadow.normalTextureId.clear();
  shadow.shadowOffset = shadowOffset;

  shadow.renderLayer = 1;

  const Uint8 a = static_cast<Uint8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);

  shadow.tint = SDL_Color{0, 0, 0, a};

  // sort within the shadow layer
  shadow.sortKey = caster.sortKey + sortKeyBias;

  renderItems.push_back(shadow);
}

void IsometricRenderSystem::flushBatches()
{
  std::stable_sort(renderItems.begin(),
                   renderItems.end(),
                   [](const RenderItem& a, const RenderItem& b)
                   {
                     if (a.renderLayer != b.renderLayer)
                       return a.renderLayer < b.renderLayer;

                     return a.sortKey < b.sortKey;
                   });

  glUseProgram(shaderProgram);

  glUniform1i(uUseTextureLocation, 1);

  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  activeBatch.vertices.clear();
  activeBatch.texture = 0;
  activeBatchTextureId.clear();

  std::string activeNormalTextureId;
  bool activeHasNormalMap = false;
  bool activeIsShadow = false;
  SDL_Color activeTint{255, 255, 255, 255};

  glm::vec3 activeLightDirection{0.0f, 0.0f, 1.0f};
  float activeLightIntensity = 1.0f;
  float activeAmbient = 0.18f;
  float activeDiffuseStrength = 0.85f;

  auto flushActiveBatch = [&]()
  {
    if (activeBatch.texture == 0 || activeBatch.vertices.empty())
      return;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, activeBatch.texture);

    GLuint normalTexture = defaultNormalTexture;
    bool useNormalMap = activeHasNormalMap;

    if (activeHasNormalMap)
    {
      GLuint uploadedNormalTexture = getOrCreateTexture(activeNormalTextureId);

      if (uploadedNormalTexture != 0)
      {
        normalTexture = uploadedNormalTexture;
      }
      else
      {
        useNormalMap = false;
      }
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, normalTexture);

    glUniform4f(uColorLocation,
                activeTint.r / 255.0f,
                activeTint.g / 255.0f,
                activeTint.b / 255.0f,
                activeTint.a / 255.0f);

    glUniform1i(uHasNormalMapLocation, useNormalMap ? 1 : 0);

    glUniform3f(uLightDirectionLocation,
                activeLightDirection.x,
                activeLightDirection.y,
                activeLightDirection.z);

    glUniform1f(uLightIntensityLocation, activeLightIntensity);
    glUniform1f(uAmbientLocation, activeAmbient);
    glUniform1f(uDiffuseStrengthLocation, activeDiffuseStrength);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(activeBatch.vertices.size() * sizeof(Vertex)),
        activeBatch.vertices.data(),
        GL_DYNAMIC_DRAW);

    glDrawArrays(
        GL_TRIANGLES, 0, static_cast<GLsizei>(activeBatch.vertices.size()));

    activeBatch.vertices.clear();
  };

  for (const auto& item : renderItems)
  {
    const bool batchBreak =
        activeBatchTextureId != item.textureId ||
        activeHasNormalMap != item.hasNormalMap ||
        activeNormalTextureId != item.normalTextureId ||
        activeIsShadow != item.isShadow || activeTint.r != item.tint.r ||
        activeTint.g != item.tint.g || activeTint.b != item.tint.b ||
        activeTint.a != item.tint.a ||
        activeLightDirection != item.lightDirection ||
        activeLightIntensity != item.lightIntensity ||
        activeAmbient != item.ambient ||
        activeDiffuseStrength != item.diffuseStrength;

    if (batchBreak)
    {
      flushActiveBatch();

      activeBatchTextureId = item.textureId;
      activeBatch.texture = getOrCreateTexture(item.textureId);

      activeHasNormalMap = item.hasNormalMap;
      activeNormalTextureId = item.normalTextureId;
      activeIsShadow = item.isShadow;
      activeTint = item.tint;

      activeLightDirection = item.lightDirection;
      activeLightIntensity = item.lightIntensity;
      activeAmbient = item.ambient;
      activeDiffuseStrength = item.diffuseStrength;
    }

    const float left = static_cast<float>(item.dest.x);
    const float right = static_cast<float>(item.dest.x + item.dest.w);
    const float top = static_cast<float>(item.dest.y);
    const float bottom = static_cast<float>(item.dest.y + item.dest.h);

    glm::vec2 q0{left, top};
    glm::vec2 q1{right, top};
    glm::vec2 q2{right, bottom};
    glm::vec2 q3{left, bottom};

    if (item.isShadow)
    {
      const float groundY = bottom;

      q0 = glm::vec2{left, groundY} + item.shadowOffset;
      q1 = glm::vec2{right, groundY} + item.shadowOffset;
      q2 = glm::vec2{right, groundY};
      q3 = glm::vec2{left, groundY};
    }

    const glm::vec2 p0 = toNdc(q0);
    const glm::vec2 p1 = toNdc(q1);
    const glm::vec2 p2 = toNdc(q2);
    const glm::vec2 p3 = toNdc(q3);

    const float u0 = static_cast<float>(item.srcRect.x) / item.textureWidth;
    const float u1 =
        static_cast<float>(item.srcRect.x + item.srcRect.w) / item.textureWidth;

    const float v0 = static_cast<float>(item.srcRect.y) / item.textureHeight;
    const float v1 = static_cast<float>(item.srcRect.y + item.srcRect.h) /
                     item.textureHeight;

    activeBatch.vertices.push_back({p0, {u0, v0}});
    activeBatch.vertices.push_back({p1, {u1, v0}});
    activeBatch.vertices.push_back({p2, {u1, v1}});

    activeBatch.vertices.push_back({p0, {u0, v0}});
    activeBatch.vertices.push_back({p2, {u1, v1}});
    activeBatch.vertices.push_back({p3, {u0, v1}});
  }

  flushActiveBatch();

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, defaultNormalTexture);

  glActiveTexture(GL_TEXTURE0);
}

void IsometricRenderSystem::setWaveTime(float time) { waveTime = time; }

void IsometricRenderSystem::setWaveEnabled(bool enabled)
{
  waveEnabled = enabled;
}

void IsometricRenderSystem::drawDebugTile(const glm::vec2& gridPosition,
                                          SDL_Color color)
{
  drawDebugTile(gridPosition, getTileElevationAt(gridPosition), color);
}

void IsometricRenderSystem::drawDebugTile(const glm::vec2& gridPosition,
                                          int elevation,
                                          SDL_Color color)
{
  initializeOpenGL();

  const glm::vec2 cameraPosition = getCameraPosition();

  const glm::vec2 screenCenter{static_cast<float>(windowWidth) / 2.0f,
                               static_cast<float>(windowHeight) / 2.0f};

  glm::vec2 screenPosition = gridToIsometric(gridPosition) -
                             gridToIsometric(cameraPosition) + screenCenter;

  screenPosition.y -= elevation * elevationStep;
  screenPosition.y -= getWaveOffset(gridPosition);

  glm::vec2 points[5] = {{screenPosition.x, screenPosition.y},
                         {screenPosition.x + tileWidth / 2.0f,
                          screenPosition.y + tileHeight / 2.0f},
                         {screenPosition.x, screenPosition.y + tileHeight},
                         {screenPosition.x - tileWidth / 2.0f,
                          screenPosition.y + tileHeight / 2.0f},
                         {screenPosition.x, screenPosition.y}};

  drawDebugLineLoop(points, 5, color);
}

void IsometricRenderSystem::setLightDirection(const glm::vec3& direction)
{
  if (!registry->hasSystem<IsometricLightingSystem>())
    return;

  registry->getSystem<IsometricLightingSystem>().setLightDirection(direction);
}

void IsometricRenderSystem::setLighting(float ambient, float diffuseStrength)
{
  if (!registry->hasSystem<IsometricLightingSystem>())
    return;

  registry->getSystem<IsometricLightingSystem>().setLighting(
      ambient, diffuseStrength);
}

glm::vec2
IsometricRenderSystem::gridToIsometric(const glm::vec2& gridPosition) const
{
  return {(gridPosition.x - gridPosition.y) *
              (static_cast<float>(tileWidth) * worldScale) / 2.0f,

          (gridPosition.x + gridPosition.y) *
              (static_cast<float>(tileHeight) * worldScale) / 2.0f};
}

glm::vec2
IsometricRenderSystem::worldDirToShadowOffset(const glm::vec2& worldDir,
                                              float length) const
{
  glm::vec2 isoDir =
      gridToIsometric(worldDir) - gridToIsometric(glm::vec2{0.0f, 0.0f});

  if (glm::length(isoDir) < 0.001f)
    return {0.0f, 0.0f};

  return glm::normalize(isoDir) * length;
}

glm::vec2 IsometricRenderSystem::isometricToGrid(const glm::vec2& iso) const
{
  const float scaledTileWidth = static_cast<float>(tileWidth) * worldScale;
  const float scaledTileHeight = static_cast<float>(tileHeight) * worldScale;

  float x =
      (iso.x / (scaledTileWidth / 2.0f) + iso.y / (scaledTileHeight / 2.0f)) *
      0.5f;

  float y =
      (iso.y / (scaledTileHeight / 2.0f) - iso.x / (scaledTileWidth / 2.0f)) *
      0.5f;

  return {x, y};
}

void IsometricRenderSystem::initializeOpenGL()
{
  if (glInitialized)
    return;

  shaderProgram = createShaderProgram();

  uTextureLocation = glGetUniformLocation(shaderProgram, "uTexture");
  uColorLocation = glGetUniformLocation(shaderProgram, "uColor");
  uUseTextureLocation = glGetUniformLocation(shaderProgram, "uUseTexture");
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

  glUseProgram(shaderProgram);

  glUniform1i(uTextureLocation, 0);
  glUniform1i(uNormalTextureLocation, 1);
  glUniform4f(uColorLocation, 1.0f, 1.0f, 1.0f, 1.0f);
  glUniform1i(uUseTextureLocation, 1);
  glUniform1i(uHasNormalMapLocation, 0);
  glUniform3f(uLightDirectionLocation, 0.0f, 0.0f, 1.0f);
  glUniform1f(uLightIntensityLocation, 1.0f);
  glUniform1f(uAmbientLocation, 0.18f);
  glUniform1f(uDiffuseStrengthLocation, 0.85f);

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

  glUseProgram(0);

  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);

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

  glGenVertexArrays(1, &debugVao);
  glGenBuffers(1, &debugVbo);

  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glInitialized = true;
}

void IsometricRenderSystem::shutdownOpenGL()
{
  if (!glInitialized)
    return;

  glBindVertexArray(0);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, 0);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  for (auto& [id, texture] : textureCache)
  {
    if (texture != 0)
      glDeleteTextures(1, &texture);
  }

  textureCache.clear();

  if (defaultNormalTexture != 0)
    glDeleteTextures(1, &defaultNormalTexture);

  defaultNormalTexture = 0;

  renderItems.clear();
  activeBatch.vertices.clear();
  activeBatch.texture = 0;
  activeBatchTextureId.clear();

  if (vbo != 0)
    glDeleteBuffers(1, &vbo);

  if (vao != 0)
    glDeleteVertexArrays(1, &vao);

  if (debugVbo != 0)
    glDeleteBuffers(1, &debugVbo);

  if (debugVao != 0)
    glDeleteVertexArrays(1, &debugVao);

  if (shaderProgram != 0)
    glDeleteProgram(shaderProgram);

  vbo = 0;
  vao = 0;
  debugVbo = 0;
  debugVao = 0;
  shaderProgram = 0;
  glInitialized = false;
}

GLuint IsometricRenderSystem::compileShader(GLenum type,
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

GLuint IsometricRenderSystem::createShaderProgram() const
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

out vec2 vUv;

void main()
{
  vUv = aUv;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

  const std::string fragmentSource = glslVersion + R"(

in vec2 vUv;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform sampler2D uNormalTexture;

uniform vec4 uColor;
uniform int uUseTexture;
uniform int uHasNormalMap;

uniform vec3 uLightDirection;
uniform float uLightIntensity;
uniform float uAmbient;
uniform float uDiffuseStrength;

void main()
{
  if (uUseTexture == 0)
  {
    FragColor = uColor;
    return;
  }

  vec4 albedo = texture(uTexture, vUv);

  if (albedo.a <= 0.0)
  {
    discard;
  }

  if (uHasNormalMap == 0)
  {
    float brightness = clamp(uAmbient + uLightIntensity, 0.0, 1.0);
    FragColor = vec4(albedo.rgb * brightness, albedo.a) * uColor;
    return;
  }

  vec3 normalSample = texture(uNormalTexture, vUv).rgb;

  float emissiveMask = smoothstep(
      0.95,
      1.0,
      min(min(normalSample.r, normalSample.g), normalSample.b));

  vec3 normal = normalSample * 2.0 - 1.0;

  if (length(normal) < 0.001)
  {
    normal = vec3(0.0, 0.0, 1.0);
  }
  else
  {
    normal = normalize(normal);
  }

  vec3 lightDir = normalize(uLightDirection);

  float ndotl = max(dot(normal, lightDir), 0.0);
  float diffuse = pow(ndotl, 0.75);

  const float maxDiffuseStrength = 0.85;

  float totalDiffuseStrength =
      clamp(uDiffuseStrength + uLightIntensity, 0.0, maxDiffuseStrength);

  float brightness =
      clamp(uAmbient + diffuse * totalDiffuseStrength, 0.0, 1.0);

  vec3 litColor = albedo.rgb * brightness;
  vec3 emissiveColor = albedo.rgb * 2.5;
  vec3 finalRgb = mix(litColor, emissiveColor, emissiveMask);

  FragColor = vec4(finalRgb, albedo.a) * uColor;
}
)";

  GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource.c_str());

  GLuint fragmentShader =
      compileShader(GL_FRAGMENT_SHADER, fragmentSource.c_str());

  if (vertexShader == 0 || fragmentShader == 0)
    return 0;

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

GLuint IsometricRenderSystem::getOrCreateTexture(const std::string& textureId)
{
  auto it = textureCache.find(textureId);

  if (it != textureCache.end())
    return it->second;

  SDL_Surface* sourceSurface = assetStore.getSurface(textureId);

  if (!sourceSurface)
    return 0;

  SDL_Surface* converted =
      SDL_ConvertSurfaceFormat(sourceSurface, SDL_PIXELFORMAT_RGBA32, 0);

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

  textureCache.emplace(textureId, texture);

  return texture;
}

void IsometricRenderSystem::drawDebugLineLoop(const glm::vec2* points,
                                              int count,
                                              SDL_Color color)
{
  std::vector<Vertex> vertices;
  vertices.reserve(static_cast<std::size_t>(count));

  for (int i = 0; i < count; i++)
    vertices.push_back({toNdc(points[i]), {0.0f, 0.0f}});

  glUseProgram(shaderProgram);
  glUniform4f(uColorLocation,
              color.r / 255.0f,
              color.g / 255.0f,
              color.b / 255.0f,
              color.a / 255.0f);
  glUniform1i(uUseTextureLocation, 0);

  glBindTexture(GL_TEXTURE_2D, 0);

  glBindVertexArray(debugVao);
  glBindBuffer(GL_ARRAY_BUFFER, debugVbo);

  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
               vertices.data(),
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

  glDrawArrays(GL_LINE_STRIP, 0, count);

  glUniform1i(uUseTextureLocation, 1);
}

glm::vec2 IsometricRenderSystem::toNdc(const glm::vec2& pixelPosition) const
{
  return {pixelPosition.x / static_cast<float>(windowWidth) * 2.0f - 1.0f,
          1.0f - pixelPosition.y / static_cast<float>(windowHeight) * 2.0f};
}

glm::vec2 IsometricRenderSystem::getCameraPosition() const
{
  if (!registry->hasSystem<CameraSystem>())
    return {0.0f, 0.0f};

  const auto& camera = registry->getSystem<CameraSystem>().getEntities();

  if (camera.empty())
    return {0.0f, 0.0f};

  return camera[0].getComponent<TransformComponent>().position;
}

glm::ivec2 IsometricRenderSystem::gridCellOf(const glm::vec2& position) const
{
  return {static_cast<int>(std::floor(position.x)),
          static_cast<int>(std::floor(position.y))};
}

bool IsometricRenderSystem::isTileEntity(const Entity& entity) const
{
  return entity.hasComponent<IsometricTile>();
}

int IsometricRenderSystem::getRenderElevationLevel(
    const Entity& entity,
    const glm::vec2& samplePosition) const
{
  if (isTileEntity(entity))
  {
    if (entity.hasComponent<ElevationComponent>())
      return entity.getComponent<ElevationComponent>().level;

    return 0;
  }

  return getTileElevationAt(samplePosition);
}

glm::vec2 IsometricRenderSystem::getGroundSamplePosition(
    const Entity& entity,
    const TransformComponent& transform) const
{
  if (isTileEntity(entity))
    return transform.position;

  return glm::floor(transform.position);
}

bool IsometricRenderSystem::tryGetTileElevationAt(const glm::vec2& position,
                                                  int& outElevation) const
{
  const glm::ivec2 targetTile = gridCellOf(position);

  bool found = false;
  int highest = 0;

  for (const auto& entity : getEntities())
  {
    if (!isTileEntity(entity))
      continue;

    if (!entity.hasComponent<ElevationComponent>())
      continue;

    const auto& transform = entity.getComponent<TransformComponent>();

    if (gridCellOf(transform.position) != targetTile)
      continue;

    const auto& elevation = entity.getComponent<ElevationComponent>();

    highest = std::max(highest, elevation.level);
    found = true;
  }

  outElevation = highest;
  return found;
}

int IsometricRenderSystem::getTileElevationAt(const glm::vec2& position) const
{
  int elevation = 0;
  tryGetTileElevationAt(position, elevation);
  return elevation;
}

float IsometricRenderSystem::getWaveOffset(const glm::vec2& gridPosition) const
{
  if (!waveEnabled)
    return 0.0f;

  return std::sin(gridPosition.x * waveFrequency +
                  gridPosition.y * waveFrequency + waveTime * waveSpeed) *
         waveAmplitude;
}

void IsometricRenderSystem::setWorldScale(float scale)
{
  worldScale = std::max(scale, 1.0f);
}

float IsometricRenderSystem::getWorldScale() const { return worldScale; }

} // namespace sfs
