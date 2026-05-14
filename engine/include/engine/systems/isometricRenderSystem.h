#pragma once

#include "engine/assetStore/assetStore.h"
#include "engine/components/transformComponent.h"
#include "engine/ecs/system.h"

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif
#include <SDL_pixels.h>
#include <SDL_rect.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

struct IsometricTile
{
};

struct ElevationComponent
{
  int level = 0;

  ElevationComponent(int level = 0);
};

class IsometricRenderSystem : public System
{
public:
  IsometricRenderSystem(AssetStore& assetStore,
                        int windowWidth,
                        int windowHeight,
                        int tileWidth,
                        int tileHeight);

  ~IsometricRenderSystem();

  void render();

  void setWaveTime(float time);
  void setWaveEnabled(bool enabled);

  void drawDebugTile(const glm::vec2& gridPosition,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void drawDebugTile(const glm::vec2& gridPosition,
                     int elevation,
                     SDL_Color color = SDL_Color{255, 255, 0, 255});

  void setLightDirection(const glm::vec3& direction);
  void setLighting(float ambient, float diffuseStrength);

  glm::vec2 gridToIsometric(const glm::vec2& gridPosition) const;
  glm::vec2 isometricToGrid(const glm::vec2& iso) const;

  IsometricRenderSystem(const IsometricRenderSystem&) = delete;
  IsometricRenderSystem& operator=(const IsometricRenderSystem&) = delete;

private:
  struct Vertex
  {
    glm::vec2 position;
    glm::vec2 uv;
  };

  struct RenderItem
  {
    std::string textureId;
    SDL_Rect srcRect;
    SDL_Rect dest;
    int textureWidth = 0;
    int textureHeight = 0;
    float sortKey = 0.0f;

    bool hasNormalMap = false;
    std::string normalTextureId;
    SDL_Rect normalSrcRect{0, 0, 0, 0};
    int normalTextureWidth = 0;
    int normalTextureHeight = 0;

    glm::vec3 lightDirection{0.0f, 0.0f, 1.0f};
    float lightIntensity = 1.0f;
    float ambient = 0.18f;
    float diffuseStrength = 0.85f;

    bool isShadow = false;
    glm::vec2 shadowOffset{0.0f};
    SDL_Color tint{255, 255, 255, 255};
  };

  struct SpriteBatch
  {
    GLuint texture = 0;
    std::vector<Vertex> vertices;
  };

private:
  void initializeOpenGL();
  void shutdownOpenGL();

  GLuint compileShader(GLenum type, const char* source) const;
  GLuint createShaderProgram() const;

  GLuint getOrCreateTexture(const std::string& textureId);

  void beginBatches();
  void submitSprite(const RenderItem& item);
  void flushBatches();

  void drawDebugLineLoop(const glm::vec2* points, int count, SDL_Color color);

  glm::vec2 toNdc(const glm::vec2& pixelPosition) const;

  glm::vec2 getCameraPosition() const;
  glm::ivec2 gridCellOf(const glm::vec2& position) const;

  bool isTileEntity(const Entity& entity) const;

  int getRenderElevationLevel(const Entity& entity,
                              const glm::vec2& samplePosition) const;

  glm::vec2 getGroundSamplePosition(const Entity& entity,
                                    const TransformComponent& transform) const;

  bool tryGetTileElevationAt(const glm::vec2& position,
                             int& outElevation) const;
  int getTileElevationAt(const glm::vec2& position) const;

  float getWaveOffset(const glm::vec2& gridPosition) const;

  void submitShadow(const RenderItem& caster,
                    const glm::vec2& shadowOffset,
                    float alpha,
                    float sortKeyBias = -0.004f);

private:
  AssetStore& assetStore;

  int windowWidth;
  int windowHeight;

  int tileWidth;
  int tileHeight;

  int elevationStep = 8;

  bool waveEnabled = true;
  float waveTime = 0.0f;
  float waveAmplitude = 6.0f;
  float waveFrequency = 0.45f;
  float waveSpeed = 3.0f;

  bool glInitialized = false;

  GLuint shaderProgram = 0;
  GLuint vao = 0;
  GLuint vbo = 0;

  GLuint debugVao = 0;
  GLuint debugVbo = 0;

  GLuint defaultNormalTexture = 0;

  GLint uTextureLocation = -1;
  GLint uColorLocation = -1;
  GLint uUseTextureLocation = -1;
  GLint uNormalTextureLocation = -1;
  GLint uHasNormalMapLocation = -1;
  GLint uLightDirectionLocation = -1;
  GLint uLightIntensityLocation = -1;
  GLint uAmbientLocation = -1;
  GLint uDiffuseStrengthLocation = -1;

  std::unordered_map<std::string, GLuint> textureCache;
  std::vector<RenderItem> renderItems;
  SpriteBatch activeBatch;
  std::string activeBatchTextureId;
};

} // namespace sfs
