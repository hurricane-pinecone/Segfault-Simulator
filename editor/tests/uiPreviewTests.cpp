#include "../../engine/tests/testHarness.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/uiModule.h"
#include "engine/runtime/rendering/renderContext.h"
#include "engine/runtime/rendering/uiDraw.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/ui/layer.h"
#include "engine/runtime/ui/ui.h"
#include "panels/uiPreviewDraw.h"

#include <glm/glm/common.hpp>
#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float4.hpp>
#include <glm/glm/ext/vector_int2.hpp>

#include <cstddef>
#include <memory>
#include <vector>

using namespace sfs;

namespace
{
// A layer showing one view, as the preview lays its views out.
class ViewLayer : public ui::Layer
{
public:
  ui::View body() override { return view; }
  ui::View view;
};

// A plain box at `rect` (pixels: x, y, width, height) in `color`.
UiQuad box(glm::vec4 rect, glm::vec4 color)
{
  UiQuad quad;
  quad.rect = rect;
  quad.color = color;
  return quad;
}

bool sameRect(const glm::vec4& a, const glm::vec4& b)
{
  return testing::approx(a.x, b.x) && testing::approx(a.y, b.y) &&
         testing::approx(a.z, b.z) && testing::approx(a.w, b.w);
}
} // namespace

int main()
{
  TEST("every quad the UI module draws should be in the preview's picture, in "
       "its colour, moved into the room round the screen")
  {
    AssetStore store;
    Scene scene{0xFFFF, SceneBackends{.assetStore = store}, "UI Preview"};
    UiModule module(scene);
    ViewLayer shown;
    shown.view = ui::VStack({
                                ui::Empty()
                                    .frame(100.0f, 50.0f)
                                    .background(ui::Color::rgb(200, 40, 40)),
                                ui::Empty()
                                    .frame(60.0f, 30.0f)
                                    .background(ui::Color::rgb(40, 40, 200)),
                            })
                     .spacing(10.0f);
    module.setLayers([&shown] { return std::vector<ui::Layer*>{&shown}; });
    module.setPixelsPerPoint(1.0f);
    const glm::vec2 screen =
        glm::ceil(module.fit(shown.view, glm::vec2(8192.0f)));
    RenderContext context;
    context.viewportSize = glm::ivec2(screen);
    module.computeCommands(context);

    std::vector<UiQuad> drawn;

    for (const UiDrawCommand& command : module.commands())
      if (command.list)
        command.list->forEach([&](const UiQuad& quad)
                              { drawn.push_back(quad); });

    CHECK(drawn.size() == 2u);

    const float room = 24.0f;
    const glm::vec2 target = screen + 2.0f * room;
    UiDrawList picture;
    previewDrawList(module.commands(), target, screen, room, picture);
    CHECK(picture.parts.empty());
    CHECK(picture.quads.size() == 2u + drawn.size());

    if (picture.quads.size() != 2u + drawn.size())
      return testing::report("uiPreviewTests");

    for (std::size_t i = 0; i < drawn.size(); ++i)
    {
      const UiQuad& quad = picture.quads[2 + i];
      const glm::vec4 moved = drawn[i].rect + glm::vec4(room, room, 0.0f, 0.0f);
      CHECK(sameRect(quad.rect, moved));
      CHECK(quad.color == drawn[i].color);
      CHECK(quad.color.a > 0.99f);
      // Inside the screen, and inside its clip.
      CHECK(quad.rect.x >= room && quad.rect.y >= room);
      CHECK(quad.rect.x + quad.rect.z <= room + screen.x + 0.01f);
      CHECK(quad.rect.y + quad.rect.w <= room + screen.y + 0.01f);
      CHECK(quad.clip.x <= quad.rect.x && quad.clip.y <= quad.rect.y);
      CHECK(quad.clip.z >= quad.rect.x + quad.rect.z);
      CHECK(quad.clip.w >= quad.rect.y + quad.rect.w);
    }

    // The red box on top, the blue one centred under it.
    CHECK(
        sameRect(picture.quads[2].rect, glm::vec4(room, room, 100.0f, 50.0f)));
    CHECK(picture.quads[2].color.r > 0.7f && picture.quads[2].color.b < 0.2f);
    CHECK(sameRect(picture.quads[3].rect,
                   glm::vec4(room + 20.0f, room + 60.0f, 60.0f, 30.0f)));
    CHECK(picture.quads[3].color.b > 0.7f && picture.quads[3].color.r < 0.2f);
  }

  TEST("the backdrops should come first: one over the whole target, one "
       "behind the screen; then a frame's layer parts, then its own quads")
  {
    const glm::vec4 red(1.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec4 green(0.0f, 1.0f, 0.0f, 1.0f);
    const glm::vec4 blue(0.0f, 0.0f, 1.0f, 0.5f);
    auto list = std::make_shared<UiDrawList>();
    list->parts.push_back(std::make_shared<const std::vector<UiQuad>>(
        std::vector<UiQuad>{box({0.0f, 0.0f, 10.0f, 10.0f}, red),
                            box({10.0f, 0.0f, 10.0f, 10.0f}, green)}));
    list->quads.push_back(box({5.0f, 5.0f, 4.0f, 4.0f}, blue));
    const std::vector<UiDrawCommand> commands{{list}, {}};

    UiDrawList picture;
    picture.quads.push_back(box({0.0f, 0.0f, 1.0f, 1.0f}, red)); // last frame's
    previewDrawList(commands,
                    glm::vec2(70.0f, 60.0f),
                    glm::vec2(50.0f, 40.0f),
                    10.0f,
                    picture);
    CHECK(picture.quads.size() == 5u);

    if (picture.quads.size() != 5u)
      return testing::report("uiPreviewTests");

    CHECK(sameRect(picture.quads[0].rect, glm::vec4(0.0f, 0.0f, 70.0f, 60.0f)));
    CHECK(
        sameRect(picture.quads[1].rect, glm::vec4(10.0f, 10.0f, 50.0f, 40.0f)));
    CHECK(
        sameRect(picture.quads[2].rect, glm::vec4(10.0f, 10.0f, 10.0f, 10.0f)));
    CHECK(picture.quads[2].color == red);
    CHECK(
        sameRect(picture.quads[3].rect, glm::vec4(20.0f, 10.0f, 10.0f, 10.0f)));
    CHECK(picture.quads[3].color == green);
    CHECK(sameRect(picture.quads[4].rect, glm::vec4(15.0f, 15.0f, 4.0f, 4.0f)));
    CHECK(picture.quads[4].color == blue);
  }

  return testing::report("uiPreviewTests");
}
