#pragma once

#include <imgui.h>

#include <filesystem>

// The Code panel's font: the monospace font Neovim's grid is drawn with, with
// fonts merged into it for the glyphs it lacks (Nerd Font icons, symbols).
// Merged glyphs are scaled to one cell of the main font, and the main font
// alone sets the line height, so merging never changes the grid's cells.

/** A code font loaded into ImGui's font atlas. */
struct CodeFont
{
  /** The font; nullptr if none could be loaded (the UI font is used then). */
  ImFont* font = nullptr;

  /** Whether it draws Nerd Font icons (merged in, or a Nerd Font itself). */
  bool icons = false;

  /**
   * For a pixel font, the ImGui font size (its line height) at which one of
   * its pixels is one screen pixel; it is crisp only at whole multiples of
   * it. 0 for an outline font, which draws well at any size.
   */
  float pixelSize = 0.0f;
};

/**
 * Load the code font into ImGui's font atlas: $SFS_EDITOR_CODE_FONT, else the
 * shipped Departure Mono, else an installed Nerd Font Mono, else the
 * platform's coding font, else the UI font's file. The shipped Symbols Nerd
 * Font is merged into it for icons; the shipped Departure Mono also gets the
 * platform's coding font merged for the symbols it lacks. Call once, after
 * the UI font is loaded.
 *
 * @param fontsFolder the editor's shipped fonts (fonts/ beside the executable)
 * @param uiFontFile the UI font's file, the last fallback (empty: none)
 * @return the font, or no font if none could be loaded
 */
CodeFont loadCodeFont(const std::filesystem::path& fontsFolder,
                      const std::filesystem::path& uiFontFile);

/**
 * The size to push the code font at for a requested size: for a pixel font,
 * the nearest whole multiple of its pixel size once ImGui's scale is applied
 * (at least one); any other font's size is the requested one.
 *
 * @param font the code font
 * @param requested the size the user chose (before ImGui's scale)
 * @param scale the factor ImGui scales pushed font sizes by
 *        (style.FontScaleMain * style.FontScaleDpi)
 * @return the size to push
 */
float codeFontSize(const CodeFont& font, float requested, float scale);
