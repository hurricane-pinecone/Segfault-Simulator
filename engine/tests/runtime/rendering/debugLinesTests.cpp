#include "../../testHarness.h"
#include "mockRenderer.h"

#include <engine/core/components/boxCollider3DComponent.h>
#include <engine/core/components/capsuleCollider3DComponent.h>
#include <engine/core/components/sphereCollider3DComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/rendering/debugLines.h>
#include <engine/core/util/debugFeatures.h>
#include <engine/runtime/assetStore/assetStore.h>
#include <engine/runtime/rendering/debug/colliderLines.h>
#include <engine/runtime/rendering/renderSystem.h>
#include <engine/runtime/sceneManager/scene.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <tuple>

using namespace sfs;
using sfs::testing3d::MockRenderer;

namespace
{
// A scene drawing through the mock renderer, the camera on the origin.
struct Harness
{
  AssetStore store;
  MockRenderer renderer;
  Scene scene{0, SceneServices{.assetStore = store, .renderer = &renderer}};
  RenderSystem& system = scene.addSystem<RenderSystem>(renderer);

  Harness()
  {
    system.camera().target = {0.0f, 0.0f, 0.0f};
    system.camera().distance = 20.0f;
  }

  Entity spawn(const glm::vec3& position, float scale = 1.0f)
  {
    Entity e = scene.createEntity();
    TransformComponent placed{position};
    placed.scale = glm::vec3(scale);
    e.addComponent<TransformComponent>(placed);
    return e;
  }

  void frame()
  {
    scene.update(0.0);
    scene.render();
  }
};

// The lowest and highest of the lines' ends.
std::pair<glm::vec3, glm::vec3> extent(std::span<const DebugLineVertex> lines)
{
  glm::vec3 low(1e9f), high(-1e9f);
  for (const DebugLineVertex& v : lines)
  {
    low = glm::min(low, v.position);
    high = glm::max(high, v.position);
  }
  return {low, high};
}
} // namespace

int main()
{
  TEST("each shape should be its outline's lines")
  {
    DebugLines lines;
    const uint32_t white = DebugLines::pack(glm::vec4(1.0f));
    lines.box(glm::mat4(1.0f), glm::vec3(0.0f), glm::vec3(1.0f), white);
    CHECK(lines.lineCount() == 12u);
    lines.clear();
    lines.sphere(glm::mat4(1.0f), glm::vec3(0.0f), 1.0f, white);
    CHECK(lines.lineCount() == 3u * DebugLines::kCircleSegments);
    lines.clear();
    lines.capsule(glm::mat4(1.0f), glm::vec3(0.0f), 0.5f, 1.0f, white);
    // Two circles, four sides, four half circles over the ends.
    CHECK(lines.lineCount() == 2u * DebugLines::kCircleSegments + 4u +
                                   4u * DebugLines::kCircleSegments / 2u);
    const auto [low, high] = extent(lines.vertices());
    CHECK(testing::approx(high.z, 1.5f, 1e-4f)); // the end, over its circle
    CHECK(testing::approx(low.z, -1.5f, 1e-4f));
    CHECK(DebugLines::pack(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)) == 0xff0000ffu);
  }

  TEST("a collider should be drawn where its entity puts it, scaled as the "
       "physics bakes it, triggers in their own colour")
  {
    Harness h;
    Entity crate = h.spawn({10.0f, 0.0f, 0.0f}, 2.0f);
    crate.addComponent<BoxCollider3DComponent>(BoxCollider3DComponent{
        .size = glm::vec3(1.0f), .offset = {0.0f, 0.0f, 0.5f}});
    Entity zone = h.spawn({-5.0f, 0.0f, 0.0f});
    zone.addComponent<SphereCollider3DComponent>(
        SphereCollider3DComponent{.radius = 2.0f, .trigger = true});

    DebugLines lines;
    colliderLines::append(crate, std::nullopt, lines);
    auto [low, high] = extent(lines.vertices());
    CHECK(testing::approx(low.x, 9.0f, 1e-4f));
    CHECK(testing::approx(high.x, 11.0f, 1e-4f));
    CHECK(testing::approx(low.z, 0.0f, 1e-4f)); // offset 0.5, scaled by 2
    CHECK(testing::approx(high.z, 2.0f, 1e-4f));
    CHECK(lines.vertices()[0].color == DebugLines::pack(colliderLines::kSolid));

    lines.clear();
    colliderLines::append(zone, std::nullopt, lines);
    std::tie(low, high) = extent(lines.vertices());
    CHECK(testing::approx(low.x, -7.0f, 1e-4f));
    CHECK(testing::approx(high.y, 2.0f, 1e-4f));
    CHECK(lines.vertices()[0].color ==
          DebugLines::pack(colliderLines::kTrigger));
  }

  TEST("colliders should be drawn only while asked for and while debug "
       "features are allowed (never in a release export)")
  {
    Harness h;
    h.spawn({0.0f, 0.0f, 0.0f})
        .addComponent<BoxCollider3DComponent>(BoxCollider3DComponent{});
    h.spawn({3.0f, 0.0f, 0.0f})
        .addComponent<CapsuleCollider3DComponent>(CapsuleCollider3DComponent{});

    debugFeatures::allow(true);
    h.frame();
    CHECK(h.renderer.linesDrawn.empty()); // off by default

    h.system.setDrawColliders(true);
    h.frame();
    CHECK(h.system.stats().colliders == 2u);
    CHECK(h.renderer.linesDrawn.size() == 2u * h.system.stats().debugLines);
    CHECK(h.system.stats().debugLines ==
          12u + 2u * DebugLines::kCircleSegments + 4u +
              4u * DebugLines::kCircleSegments / 2u);

    // Game code turns them off over the scene's setting, then back.
    h.system.forceDrawColliders(false);
    h.frame();
    CHECK(h.renderer.linesDrawn.empty());
    h.system.forceDrawColliders(true);
    h.frame();
    CHECK(!h.renderer.linesDrawn.empty());

    // A release export: nothing, whatever the scene and game code say.
    debugFeatures::allow(false);
    CHECK(!debugFeatures::enabled());
    CHECK(!h.system.effectiveDrawColliders());
    h.system.debugLines().line(glm::vec3(0.0f), glm::vec3(1.0f), 0xffffffffu);
    h.frame();
    CHECK(h.renderer.linesDrawn.empty());
    CHECK(h.system.debugLines().empty()); // emptied undrawn, not kept
    debugFeatures::allow(true);
    h.system.forceDrawColliders(std::nullopt);
  }

  TEST("game code's lines should be drawn for one frame")
  {
    Harness h;
    debugFeatures::allow(true);
    h.system.debugLines().line(
        glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec4(1.0f));
    h.frame();
    CHECK(h.renderer.linesDrawn.size() == 2u);
    h.frame();
    CHECK(h.renderer.linesDrawn.empty());
  }

  TEST("500 colliders should cost little drawn, and nothing off")
  {
    Harness h;
    debugFeatures::allow(true);
    for (int i = 0; i < 500; ++i)
    {
      Entity e = h.spawn({float(i % 25) * 3.0f, float(i / 25) * 3.0f, 0.0f});
      if (i % 3 == 0)
        e.addComponent<BoxCollider3DComponent>(BoxCollider3DComponent{});
      else if (i % 3 == 1)
        e.addComponent<SphereCollider3DComponent>(SphereCollider3DComponent{});
      else
        e.addComponent<CapsuleCollider3DComponent>(
            CapsuleCollider3DComponent{});
    }
    h.frame();
    // The render frame's time, the best of a few runs (the mock renderer
    // only copies the lines).
    const auto timed = [&]
    {
      double best = 1e9;
      for (int run = 0; run < 5; ++run)
      {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i)
          h.scene.render();
        best = std::min(best,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                                .count() /
                            20.0);
      }
      return best;
    };
    h.system.setDrawColliders(false);
    const double off = timed();
    h.system.setDrawColliders(true);
    const double on = timed();
    CHECK(h.system.stats().colliders == 500u);
    std::printf("500 colliders (%zu lines): render %.3f ms drawn, %.3f ms "
                "off\n",
                h.system.stats().debugLines,
                on,
                off);
    CHECK(on - off < 5.0); // generous: a debug build's, on any machine
  }

  return testing::report("debugLinesTests");
}
