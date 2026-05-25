#pragma once

#include "engine/ecs/ecs.h" // IWYU pragma: keep
#include "engine/ecs/entity.h"
#include "engine/rendering/commands/commands.h"
#include "engine/rendering/isometricRenderContext.h"
#include "engine/rendering/renderProvider.h"

namespace sfs
{

class IsometricWaterSystem
    : public System,
      public RenderProvider<IsometricRenderContext, QuadCommand>
{
public:
  void computeCommands(const IsometricRenderContext& context) override;

protected:
  void create() override;

private:
  void constructRenderCommands(const IsometricRenderContext& context,
                               const Entity& e);
};

} // namespace sfs
