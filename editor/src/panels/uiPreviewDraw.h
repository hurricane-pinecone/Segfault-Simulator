#pragma once

#include <glm/glm/ext/vector_float2.hpp>

#include <vector>

namespace sfs
{
struct UiDrawCommand;
struct UiDrawList;
} // namespace sfs

/**
 * The UI Preview's picture of a frame of UI, back to front: a backdrop over
 * the whole target, a plain one behind the screen (UI is drawn over a game,
 * not the panel's dark), then every quad the UI module drew (its layers' parts
 * and its own quads), moved into the room round the screen.
 *
 * @param commands the UI module's commands for the frame
 * @param target   the picture's size in pixels: the screen and its room
 * @param screen   the screen's size in pixels
 * @param room     the room round the screen, in pixels
 * @param list     cleared, then filled with the picture's quads
 */
void previewDrawList(const std::vector<sfs::UiDrawCommand>& commands,
                     glm::vec2 target,
                     glm::vec2 screen,
                     float room,
                     sfs::UiDrawList& list);
