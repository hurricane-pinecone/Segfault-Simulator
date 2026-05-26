#pragma once

#include "SDL_pixels.h"
#include <cstdint>

namespace sfs
{

struct Color
{
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  uint8_t a = 255;

  constexpr SDL_Color toSDL() const { return SDL_Color{r, g, b, a}; }
};

namespace Colors
{
inline constexpr Color White{255, 255, 255, 255};
inline constexpr Color Black{0, 0, 0, 255};

inline constexpr Color Red{255, 0, 0, 255};
inline constexpr Color Green{0, 255, 0, 255};
inline constexpr Color Blue{0, 0, 255, 255};

inline constexpr Color Yellow{255, 255, 0, 255};
inline constexpr Color Cyan{0, 255, 255, 255};
inline constexpr Color Magenta{255, 0, 255, 255};

inline constexpr Color Orange{255, 165, 0, 255};
inline constexpr Color Purple{128, 0, 128, 255};
inline constexpr Color Pink{255, 105, 180, 255};

inline constexpr Color Gray{128, 128, 128, 255};
inline constexpr Color LightGray{192, 192, 192, 255};
inline constexpr Color DarkGray{64, 64, 64, 255};

inline constexpr Color Brown{139, 69, 19, 255};
inline constexpr Color Lime{50, 205, 50, 255};
inline constexpr Color Navy{0, 0, 128, 255};
inline constexpr Color Turqoise{55, 155, 210, 70};

inline constexpr Color Transparent{0, 0, 0, 0};
} // namespace Colors

} // namespace sfs
