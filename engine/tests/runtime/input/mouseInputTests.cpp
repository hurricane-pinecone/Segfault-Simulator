#include "../../testHarness.h"

#include <engine/runtime/input/mouseInput.h>

using namespace sfs;

int main()
{
  TEST("the cursor should be in view only inside the view's size")
  {
    MouseInput mouse;
    mouse.setViewSize(800, 600);
    mouse.setPosition(0, 0);
    CHECK(mouse.inView());
    mouse.setPosition(799, 599);
    CHECK(mouse.inView());
    mouse.setPosition(800, 10);
    CHECK(!mouse.inView());
    mouse.setPosition(-3, 10); // left of an editor viewport, say
    CHECK(!mouse.inView());
  }

  TEST("with no view size yet, nothing should be in view")
  {
    MouseInput mouse;
    mouse.setPosition(5, 5);
    CHECK(!mouse.inView());
  }

  TEST("a button past those tracked (a many-button mouse) should be ignored")
  {
    MouseInput mouse;
    const auto extra = static_cast<MouseButton>(200);
    mouse.beginFrame();
    mouse.pressButton(extra);
    mouse.update();
    CHECK(!mouse.mouseHeld(extra));
    CHECK(!mouse.mousePressed(extra));
    mouse.releaseButton(extra);
    CHECK(!mouse.mouseReleased(extra));
    CHECK(!mouse.mouseHeld(static_cast<MouseButton>(-1)));
  }

  return testing::report("mouseInputTests");
}
