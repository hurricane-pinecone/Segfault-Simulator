#include "../../testHarness.h"

#include <engine/core/rendering/renderPass.h>
#include <engine/core/rendering/vertices.h>
#include <engine/runtime/rendering/util/clipDepth.h>
#include <engine/runtime/rendering/commands/commands.h>

#include <variant>
#include <vector>

using namespace sfs;

namespace
{
QuadCommand quadAt(float depth, RenderPass pass = RenderPass::Objects)
{
  QuadCommand c;
  c.order.depth = depth;
  c.order.pass = pass;
  return c;
}
} // namespace

int main()
{
  TEST("a higher sort-key should map nearer the camera (smaller clip z)")
  {
    std::vector<AnyRenderCommand> cmds;
    cmds.push_back(quadAt(0.0f));  // far
    cmds.push_back(quadAt(10.0f)); // near

    const auto [minKey, maxKey] = assignClipDepth(cmds);
    CHECK(testing::approx(minKey, 0.0f));
    CHECK(testing::approx(maxKey, 10.0f));

    const float farZ = std::get<QuadCommand>(cmds[0]).quad.z;
    const float nearZ = std::get<QuadCommand>(cmds[1]).quad.z;
    CHECK(testing::approx(farZ, 0.9f));   // lowest key -> far plane
    CHECK(testing::approx(nearZ, -0.9f)); // highest key -> near plane
    CHECK(nearZ < farZ);
  }

  TEST("a single command should land mid-window")
  {
    std::vector<AnyRenderCommand> cmds;
    cmds.push_back(quadAt(5.0f)); // degenerate range -> t = 0.5

    assignClipDepth(cmds);
    CHECK(testing::approx(std::get<QuadCommand>(cmds[0]).quad.z, 0.0f));
  }

  TEST("UI commands should not widen the depth range")
  {
    std::vector<AnyRenderCommand> cmds;
    cmds.push_back(quadAt(0.0f));
    cmds.push_back(quadAt(10.0f));
    cmds.push_back(quadAt(1000.0f, RenderPass::UI)); // excluded from the range

    const auto [minKey, maxKey] = assignClipDepth(cmds);
    CHECK(testing::approx(minKey, 0.0f));
    CHECK(testing::approx(maxKey, 10.0f)); // not 1000
  }

  TEST("a sprite's top edge should sort nearer than its feet")
  {
    std::vector<AnyRenderCommand> cmds;
    LitQuadCommand sprite;
    sprite.order.depth = 0.0f;     // feet sort-key
    sprite.quad.depthSpan = 5.0f;  // top reaches +5 toward the camera
    cmds.push_back(sprite);

    assignClipDepth(cmds);
    const LitQuad& q = std::get<LitQuadCommand>(cmds[0]).quad;
    CHECK(testing::approx(q.z, 0.9f));     // feet = farthest key in the frame
    CHECK(testing::approx(q.zTop, -0.9f)); // top = nearest
    CHECK(q.zTop < q.z);
  }

  TEST("merged surface vertices should remap per vertex, decals untouched")
  {
    std::vector<AnyRenderCommand> cmds;
    SurfaceCommand surface;
    SurfaceVertex v0;
    v0.z = 0.0f;
    SurfaceVertex v1;
    v1.z = 10.0f;
    surface.vertices = {v0, v1};
    cmds.push_back(surface);
    cmds.push_back(DecalDrawCommand{}); // owns its depth; must be skipped

    const auto [minKey, maxKey] = assignClipDepth(cmds);
    CHECK(testing::approx(minKey, 0.0f));  // decal didn't touch the range
    CHECK(testing::approx(maxKey, 10.0f));

    const SurfaceCommand& out = std::get<SurfaceCommand>(cmds[0]);
    CHECK(testing::approx(out.vertices[0].z, 0.9f));
    CHECK(testing::approx(out.vertices[1].z, -0.9f));
  }

  return testing::report("clipDepthTests");
}
