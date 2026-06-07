#pragma once

#include "engine/core/ecs/registry.h" // IWYU pragma: keep
#include "engine/core/rendering/util/isometric/geometry.h"
#include "engine/runtime/rendering/util/isometric/isometricLightingUtils.h"
#include "engine/runtime/rendering/commands/shadowCommands.h"
#include "engine/runtime/rendering/isometricRenderContext.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include <atomic>
#include <vector>

namespace sfs
{

extern std::atomic<uint64_t> gShadowPathChecks;
extern std::atomic<uint64_t> gShadowTilesTraversed;

/**
 * Render module that builds projected sun-shadow quads for terrain edges,
 * occluding them against the render context's terrain elevation grid. It pulls
 * tiles from a Registry view and emits batched shadow commands. It contributes
 * nothing while terrain self-shadows through block geometry, so its emit() is a
 * no-op when the context's geometryActive flag is set.
 */
class TerrainShadow
    : public CommandModule<IsometricRenderContext, TerrainShadowBatchCommand>
{
public:
  TerrainShadow() = default;
  explicit TerrainShadow(IsometricShadowSettings settings)
      : m_shadowSettings(settings) {};

  void init(const ModuleInit& m) override { registry = m.registry; }

  void computeCommands(const IsometricRenderContext& context) override;

  // The block-geometry render style self-shadows via the in-shader heightmap
  // march, so projected quads must not also draw.
  void emit(const IsometricRenderContext& context,
            std::vector<AnyRenderCommand>& out) override
  {
    if (context.geometryActive)
      return;

    CommandModule::emit(context, out);
  }

  /** Invalidate the cached edge geometry so it rebuilds next frame. */
  void markTerrainDirty();

  IsometricShadowSettings& shadowSettings() { return m_shadowSettings; }

  std::vector<ModuleSetting>
  settings(const IsometricRenderContext& context) override
  {
    // Projected terrain shadows only render in the billboard style; under block
    // geometry the in-shader heightmap march replaces them and reads the
    // heightmap directly, so these values do nothing there.
    if (context.geometryActive)
      return {};

    return {
        settings::floatRange(
            "Terrain shadow length",
            0.0f,
            5.0f,
            [this] { return m_shadowSettings.terrainShadowMaxLength; },
            [this](float v)
            {
              m_shadowSettings.terrainShadowMaxLength = v;
              markTerrainDirty();
            }),
        settings::floatRange(
            "Terrain alpha",
            0.0f,
            1.0f,
            [this] { return m_shadowSettings.terrainShadowAlpha; },
            [this](float v)
            {
              m_shadowSettings.terrainShadowAlpha = v;
              markTerrainDirty();
            }),
    };
  }

private:
  void computeTerrainShadows(const IsometricRenderContext& context);

  void setTerrainShadowMaxLength(float length);
  void setTerrainShadowAlpha(float alpha);

  void constructTerrainEdgeShadowProjectedClipped(
      const IsometricRenderContext& renderContext,
      std::vector<Quad>& outQuads,
      const TerrainShadowEdge& edge,
      const glm::vec2& shadowDir,
      float sunHeight,
      float maxShadowLength,
      float alpha);

  bool terrainShadowPathBlocked(const IsometricRenderContext& renderContext,
                                const TerrainShadowEdge& edge,
                                const glm::ivec2& receiverTile,
                                const glm::vec2& shadowDir) const;

  bool shouldCastTerrainShadow(const TerrainShadowEdge& edge,
                               const glm::vec2& shadowDir);

  std::vector<TerrainShadowEdge>
  mergeTerrainShadowEdges(const std::vector<TerrainShadowEdge>& input) const;

  std::vector<TerrainShadowEdge>
  getTerrainShadowEdges(const IsometricRenderContext& context) const;

  void buildTerrainEdgeShadowItems(const IsometricRenderContext& context,
                                   const glm::vec2& shadowDir,
                                   float sunHeight,
                                   float alpha);

  float calculateTerrainShadowLength(const TerrainShadowEdge& edge,
                                     float sunHeight,
                                     float maxShadowLength);

  void emitTileShadow(const IsometricRenderContext& context,
                      std::vector<Quad>& outQuads,
                      const glm::ivec2& tile,
                      int elevation,
                      const ClippedPolygon& poly,
                      float alpha);

private:
  Registry* registry = nullptr;

  struct TerrainShadowCache
  {
    std::vector<TerrainShadowEdge> edges;
    TileBounds edgeTileBounds;

    bool edgesDirty = true;
    bool itemsDirty = true;

    float alpha = -1.0f;

    glm::vec3 sunDir{999.0f, 999.0f, 999.0f};
    glm::vec2 isoCameraPosition{999.0f, 999.0f};
    float zoom = -1.0f;
  };

  TerrainShadowCache m_cache;
  IsometricShadowSettings m_shadowSettings;

  // Persistent per-worker-range scratch buffers, reused across rebuilds so we
  // don't reallocate them every frame (clear() keeps capacity).
  std::vector<std::vector<Quad>> m_shadowRangeQuads;
};
} // namespace sfs
