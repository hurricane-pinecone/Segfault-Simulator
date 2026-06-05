#pragma once

#include <SDL_mouse.h>
#include <array>
#include <glm/glm/vec2.hpp>

namespace sfs
{

enum class MouseButton : int
{
  Left = SDL_BUTTON_LEFT,
  Middle = SDL_BUTTON_MIDDLE,
  Right = SDL_BUTTON_RIGHT,
  X1 = SDL_BUTTON_X1,
  X2 = SDL_BUTTON_X2
};

class MouseInput
{
public:
  void beginFrame();
  void update();

  void pressButton(MouseButton button);
  void releaseButton(MouseButton button);

  bool mouseHeld(MouseButton button) const;
  bool mousePressed(MouseButton button) const;
  bool mouseReleased(MouseButton button) const;

  bool dragging(MouseButton button) const;
  bool dragStarted(MouseButton button) const;
  bool dragReleased(MouseButton button) const;
  void processDrag();

  glm::vec2 getPosition() const;
  glm::vec2 getDelta() const;
  glm::vec2 getDragDelta() const;

  void setPosition(int x, int y);
  int getScrollX() const;
  int getScrollY() const;
  void addScroll(int x, int y);

private:
  std::array<bool, 8> currentButtons{};
  std::array<bool, 8> previousButtons{};

  glm::vec2 position{};
  glm::vec2 previousPosition{};
  glm::vec2 dragStartPosition{};

  bool dragActive = false;
  bool dragStartedThisFrame = false;
  bool dragReleasedThisFrame = false;

  float dragThreshold = 4.0f;

  int scrollX = 0;
  int scrollY = 0;
};

} // namespace sfs
