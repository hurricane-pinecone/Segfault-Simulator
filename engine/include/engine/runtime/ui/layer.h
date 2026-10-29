#pragma once

#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/ui/ui.h"

namespace sfs::ui
{

/**
 * A layer of a game's UI: a script (put it on an entity, as any game object)
 * whose body() describes what it shows, from the game's state, every frame
 * while the scene plays. Layers are drawn over the scene in order, and clicks
 * on them aren't the game's (MouseInput::blocked()).
 *
 *   class Hud : public sfs::ui::Layer
 *   {
 *   public:
 *     sfs::ui::View body() override
 *     {
 *       using namespace sfs::ui;
 *       return Text("Coins: " + std::to_string(coins))
 *           .padding().background(Panel()).align(Alignment::TopLeading);
 *     }
 *     int coins = 0;
 *   };
 */
class Layer : public GameObject
{
public:
  /** What it shows now (laid out on the whole screen, placed by its align()).
   */
  virtual View body() = 0;

  /** Drawn over layers with lower orders, and clicked first. */
  int order = 0;
  /** Whether it's shown at all. */
  bool visible = true;
};

} // namespace sfs::ui
