#pragma once

#include <cstdint>

namespace sfs
{

// How a translucent draw combines with what is already in the framebuffer.
enum class BlendMode : uint8_t
{
  // Standard transparency: src.rgb * src.a + dst * (1 - src.a). Smoke, blood,
  // dust -- anything that should darken/replace what is behind it.
  Alpha = 0,

  // Additive glow: src.rgb * src.a + dst. Fire, magic, sparks -- light that
  // accumulates and never darkens the background. Order-independent.
  Additive,
};

} // namespace sfs
