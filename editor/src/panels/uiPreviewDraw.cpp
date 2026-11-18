#include "panels/uiPreviewDraw.h"

#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/uiDraw.h"

#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/ext/vector_float4.hpp>

#include <vector>

void previewDrawList(const std::vector<sfs::UiDrawCommand>& commands,
                     glm::vec2 target,
                     glm::vec2 screen,
                     float room,
                     sfs::UiDrawList& list)
{
  list.quads.clear();
  list.parts.clear();

  sfs::UiQuad back;
  back.rect = {0.0f, 0.0f, target.x, target.y};
  back.color = {0.11f, 0.11f, 0.12f, 1.0f};
  list.quads.push_back(back);
  back.rect = {room, room, screen.x, screen.y};
  back.color = {0.36f, 0.42f, 0.36f, 1.0f};
  list.quads.push_back(back);

  for (const sfs::UiDrawCommand& command : commands)
  {
    if (!command.list)
      continue;

    command.list->forEach(
        [&](sfs::UiQuad quad)
        {
          quad.rect.x += room;
          quad.rect.y += room;
          quad.clip += glm::vec4(room);
          list.quads.push_back(quad);
        });
  }
}
