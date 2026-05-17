#pragma once

#include "SDL2/SDL_rect.h"
#include "SDL2/SDL_surface.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include <string>
#include <unordered_map>
#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif

namespace sfs
{

class OpenGLQuadRenderer
{
public:
  static constexpr int MaxShaderLights = 16;

  struct Vertex
  {
    glm::vec2 position;
    glm::vec2 uv;
    glm::vec2 worldPosition;
  };

  struct QuadDrawCommand
  {
    unsigned int texture = 0;

    SDL_Rect srcRect{0, 0, 0, 0};
    SDL_Rect destRect{0, 0, 0, 0};

    int textureWidth = 0;
    int textureHeight = 0;

    SDL_Color tint{255, 255, 255, 255};
  };

  struct FreeformQuadDrawCommand
  {
    unsigned int texture = 0;

    SDL_Rect srcRect{0, 0, 0, 0};

    int textureWidth = 0;
    int textureHeight = 0;

    glm::vec2 points[4];

    SDL_Color tint{255, 255, 255, 255};
  };

  struct LitQuadDrawCommand : QuadDrawCommand
  {
    bool hasNormalMap = false;
    unsigned int normalTexture = 0;

    glm::vec3 lightDirection{0.0f, 0.0f, 1.0f};
    float lightIntensity = 1.0f;
    float ambient = 0.18f;
    float diffuseStrength = 0.85f;

    glm::vec3 lightColor{1.0f, 1.0f, 1.0f};
    glm::vec2 worldPoints[4];
    int lightCount = 0;
    glm::vec2 lightPositions[MaxShaderLights];
    glm::vec3 lightColors[MaxShaderLights];
    float lightIntensities[MaxShaderLights];
    float lightRadii[MaxShaderLights];
    float lightHeights[MaxShaderLights];
  };

  struct TerrainShadowDrawCommand
  {
    glm::vec2 edgeA{0.0f, 0.0f};
    glm::vec2 edgeB{0.0f, 0.0f};
    glm::vec2 shadowDir{0.0f, 1.0f};

    float shadowLength = 0.0f;
    int elevation = 0;

    float zoom = 1.0f;
    glm::vec2 isoCameraPosition{0.0f, 0.0f};
    glm::vec2 screenCenter{0.0f, 0.0f};

    int tileWidth = 0;
    int tileHeight = 0;
    int elevationStep = 0;
    float worldScale = 1.0f;

    SDL_Color tint{0, 0, 0, 120};
  };

  OpenGLQuadRenderer(int windowWidth, int windowHeight);
  ~OpenGLQuadRenderer();

  void initialize();
  void shutdown();

  unsigned int getOrCreateTexture(const std::string& textureId,
                                  SDL_Surface* surface);

  unsigned int uploadSurfaceTexture(SDL_Surface* surface);
  void deleteTexture(unsigned int texture);

  void drawQuad(const QuadDrawCommand& command); // Text, UI, simple sprites
  void drawFreeformQuad(const FreeformQuadDrawCommand& command); // Shadows
  void
  drawLitQuad(const LitQuadDrawCommand& command); // Normalised sprites (ie,
                                                  // sprites that light affects)
  void drawTerrainShadow(const TerrainShadowDrawCommand& command);
  void drawLineLoop(const glm::vec2* points, int count, SDL_Color color);

  void setViewportSize(int width, int height);

private:
  glm::vec2 toNdc(const glm::vec2& pixelPosition) const;

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

  unsigned int compileShader(unsigned int type, const char* source) const;
  unsigned int createShaderProgram() const;

private:
  int windowWidth = 0;
  int windowHeight = 0;

  bool initialized = false;

  unsigned int shaderProgram = 0;
  unsigned int vao = 0;
  unsigned int vbo = 0;

  unsigned int defaultNormalTexture = 0;

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
  GLint uTerrainShadowModeLocation = -1;
  GLint uTerrainShadowDirLocation = -1;
  GLint uTerrainShadowLengthLocation = -1;
  GLint uTerrainShadowElevationLocation = -1;
  GLint uTerrainShadowCameraIsoLocation = -1;
  GLint uTerrainShadowScreenCenterLocation = -1;
  GLint uTerrainShadowViewportSizeLocation = -1;
  GLint uTerrainShadowZoomLocation = -1;
  GLint uTerrainShadowTileSizeLocation = -1;
  GLint uTerrainShadowElevationStepLocation = -1;
  GLint uTerrainShadowWorldScaleLocation = -1;

  std::unordered_map<std::string, unsigned int> textureCache;
};

} // namespace sfs
