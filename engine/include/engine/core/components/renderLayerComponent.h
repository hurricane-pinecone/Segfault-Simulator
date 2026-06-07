#pragma once

namespace sfs
{

/**
 * Optional 2D draw-order hint for the flat render path. Sprites are ordered by
 * layer first, then by world Y within a layer (lower Y draws behind), so a
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
