#pragma once

namespace sfs
{

/**
 * Optional draw-order hint for a 2D game's sprites. Higher layers draw in
 * front (within a layer, by Y too where the game's Space2D sorts by it), so a
 * parallax background sits on a low layer and foreground decoration on a high
 * one. An entity without this component is treated as layer 0.
 *
 * @param int layer - draw layer (higher draws in front), default 0
 */
struct RenderLayerComponent
{
  int layer = 0;

  RenderLayerComponent() = default;
  explicit RenderLayerComponent(int layer) : layer(layer) {}
};

} // namespace sfs
