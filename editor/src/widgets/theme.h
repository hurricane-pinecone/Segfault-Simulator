#pragma once

#include "imgui.h"

// The editor's look: soft warm greys with a desaturated sage-green accent,
// rounded corners and a proportional UI font, in place of ImGui's defaults.
namespace theme
{

// Colours panels draw with themselves (sRGB, like ImGui's).
inline constexpr ImU32 kAccent = IM_COL32(170, 214, 160, 255);     // pastel sage
inline constexpr ImU32 kAccentSoft = IM_COL32(170, 214, 160, 60);  // selection washes
inline constexpr ImU32 kTile = IM_COL32(58, 62, 60, 255);           // card backgrounds
inline constexpr ImU32 kTileHovered = IM_COL32(68, 73, 70, 255);
inline constexpr ImU32 kIconInk = IM_COL32(210, 214, 206, 255);     // drawn icons

/** Style colours, rounding and spacing. Call once, after ImGui is set up. */
void apply();

/**
 * Make a proportional system font the UI's (the first font in the atlas), if
 * one is found; else ImGui's built-in one stays. Call before any other font
 * is added.
 */
void loadUiFont();

} // namespace theme
