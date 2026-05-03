#include <engine/input/mouseInput.h>

#include <SDL.h>
#include <glm/geometric.hpp>

namespace sfs
{

void MouseInput::beginFrame()
{
  previousButtons = currentButtons;
  previousPosition = position;

  dragStartedThisFrame = false;
  dragReleasedThisFrame = false;

  scrollX = 0;
  scrollY = 0;
}

void MouseInput::update()
{
  int x, y;
  SDL_GetMouseState(&x, &y);

  setPosition(x, y);
  processDrag();
}

void MouseInput::pressButton(MouseButton button)
{
  currentButtons[static_cast<int>(button)] = true;

  dragStartPosition = position;
  dragActive = false;
  dragStartedThisFrame = false;
  dragReleasedThisFrame = false;
}

void MouseInput::releaseButton(MouseButton button)
{
  currentButtons[static_cast<int>(button)] = false;

  if (dragActive)
  {
    dragReleasedThisFrame = true;
  }

  dragActive = false;
}

void MouseInput::setPosition(int x, int y)
{
  position = {static_cast<float>(x), static_cast<float>(y)};
}

void MouseInput::processDrag()
{
  if (!mouseHeld(MouseButton::Left))
  {
    return;
  }

  if (dragActive)
  {
    return;
  }

  glm::vec2 delta = position - dragStartPosition;

  if (glm::length(delta) >= dragThreshold)
  {
    dragActive = true;
    dragStartedThisFrame = true;
  }
}

bool MouseInput::mouseHeld(MouseButton button) const
{
  return currentButtons[static_cast<int>(button)];
}

bool MouseInput::mousePressed(MouseButton button) const
{
  int b = static_cast<int>(button);
  return currentButtons[b] && !previousButtons[b];
}

bool MouseInput::mouseReleased(MouseButton button) const
{
  int b = static_cast<int>(button);
  return !currentButtons[b] && previousButtons[b];
}

bool MouseInput::dragging(MouseButton button) const
{
  return mouseHeld(button) && dragActive;
}

bool MouseInput::dragStarted(MouseButton button) const
{
  return mouseHeld(button) && dragStartedThisFrame;
}

bool MouseInput::dragReleased(MouseButton button) const
{
  return dragReleasedThisFrame;
}

glm::vec2 MouseInput::getPosition() const { return position; }

glm::vec2 MouseInput::getDelta() const { return position - previousPosition; }

glm::vec2 MouseInput::getDragDelta() const
{
  return position - dragStartPosition;
}

int MouseInput::getScrollX() const { return scrollX; }

int MouseInput::getScrollY() const { return scrollY; }

void MouseInput::addScroll(int x, int y)
{
  scrollX += x;
  scrollY += y;
}

} // namespace sfs
