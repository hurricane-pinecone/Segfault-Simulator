#pragma once

#include "engine/Color/Color.h"
#include "engine/ecs/ecs.h" // IWYU pragma: keep
#include "engine/ecs/entity.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderProvider.h"

namespace sfs
{

struct WaterCell
{
  glm::ivec2 tile{};
  int elevation = 0;
  float depth = 0.0f;
};

struct WaterSurfaceBuild
{
  std::vector<WaterCell> cells;
  glm::ivec2 minTile{};
  glm::ivec2 maxTile{};
};

class IsometricWaterSystem
    : public System,
      public RenderProvider<IsometricRenderContext, SurfaceCommand>
{
public:
  void computeCommands(const IsometricRenderContext& context) override;

protected:
  void create() override;

private:
  WaterSurfaceBuild
  collectWaterSurfaceBuild(const IsometricRenderContext& context) const;
  SurfaceCommand createWaterSurfaceCommand() const;

  void buildWaterSurfaceMesh(const IsometricRenderContext& context,
                             const WaterSurfaceBuild& build,
                             SurfaceCommand& command) const;

  uint32_t addSurfaceVertex(const IsometricRenderContext& context,
                            const glm::ivec2& gridPoint,
                            int waterElevation,
                            float depth,
                            SurfaceCommand& command) const;

  float sampleSurfaceVertexDepth(const glm::ivec2& gridPoint,
                                 const glm::ivec2& minTile,
                                 int cellWidth,
                                 int cellHeight,
                                 const std::vector<float>& cellDepths) const;
};

} // namespace sfs
