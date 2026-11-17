#pragma once

#include <glm/glm/ext/vector_float2.hpp>
#include <SDL_mouse.h>

#include <array>

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
  void addScroll(int x, int y);
  void beginFrame();
  /**
   * The UI has the mouse (it's over a UI layer, or pressed on one): buttons,
   * drags and scrolling read as untouched to the game from here on this
   * frame. blocked() says so, e.g. to hide a cursor marker under UI.
   */
  void block();
  bool blocked() const { return isBlocked; }
  bool dragging(MouseButton button) const;
  bool dragReleased(MouseButton button) const;
  bool dragStarted(MouseButton button) const;
  glm::vec2 getDelta() const;
  glm::vec2 getDragDelta() const;
  // Positions are in pixels from the top-left of where the scene is drawn
  // (the window, or the editor's viewport), so they can go negative or past
  // the view's size when the cursor is outside it.
  glm::vec2 getPosition() const;
  int getScrollX() const;
  int getScrollY() const;
  glm::vec2 getViewSize() const { return viewSize; }

  /** Whether the cursor is over the scene's view. */
  bool inView() const;
  bool mouseHeld(MouseButton button) const;
  bool mousePressed(MouseButton button) const;
  bool mouseReleased(MouseButton button) const;
  void pressButton(MouseButton button);
  void processDrag();
  void releaseButton(MouseButton button);
  void setPosition(int x, int y);

  /** The size of the view positions are measured in (set by the game loop). */
  void setViewSize(int width, int height);
  void update();

private:
  std::array<bool, 8> currentButtons{};
  std::array<bool, 8> previousButtons{};

  glm::vec2 position{};
  glm::vec2 viewSize{};
  glm::vec2 previousPosition{};
  glm::vec2 dragStartPosition{};

  bool dragActive = false;
  bool dragStartedThisFrame = false;
  bool dragReleasedThisFrame = false;

  float dragThreshold = 4.0f;

  int scrollX = 0;
  int scrollY = 0;
  bool isBlocked = false;
};

} // namespace sfs
