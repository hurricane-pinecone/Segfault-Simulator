#pragma once

#include "engine/runtime/ui/layout.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

struct _TTF_Font;

namespace sfs::ui
{

/**
 * Text for the UI, from TrueType fonts (SDL_ttf): each font at each pixel
 * size is a texture of its glyphs, baked as text first needs them (so any
 * language's letters work), which a renderer draws from.
 */
class FontAtlas : public TextShaper
{
public:
  /**
   * `resolve` turns a Font's file into a path to open (empty if there's no
   * such font: then the fallback, `fallbackFile`, is used).
   */
  FontAtlas(std::function<std::string(const std::string&)> resolve,
            std::string fallbackFile);
  ~FontAtlas() override;

  FontAtlas(const FontAtlas&) = delete;
  FontAtlas& operator=(const FontAtlas&) = delete;

  /**
   * A new frame of text starts: faces (a font at a size) unused through the
   * last frame may be dropped, when there are more than kMostFaces (UI
   * scaled smoothly would otherwise make a texture at every size it passes).
   * Text drawn before this call mustn't be drawn after it.
   */
  void beginFrame();

  /** The faces kept beyond those in use. */
  static constexpr std::size_t kMostFaces = 24;

  /** How many faces (a font at a size) it has now. */
  std::size_t faceCount() const { return m_faces.size(); }

  float lineHeight(const std::string& font, float pixels) override;
  float
  width(const std::string& font, float pixels, std::string_view line) override;
  void draw(const std::string& font,
            float pixels,
            std::string_view line,
            glm::vec2 at,
            glm::vec4 color,
            std::vector<UiQuad>& out) override;

private:
  struct Glyph
  {
    glm::ivec2 at{0, 0};   // in the texture
    glm::ivec2 size{0, 0}; // 0: nothing to draw (a space)
    float advance = 0.0f;
  };

  struct Face
  {
    _TTF_Font* font = nullptr;
    float lineHeight = 0.0f;
    UiTexture texture;
    glm::ivec2 shelf{1, 1}; // where the next glyph goes
    int shelfHeight = 0;
    std::unordered_map<char32_t, Glyph> glyphs;
    uint64_t lastUsed = 0; // the frame it was last used in
  };

  Face* face(const std::string& font, float pixels);
  const Glyph& glyph(Face& face, char32_t codepoint);

  std::function<std::string(const std::string&)> m_resolve;
  std::string m_fallback;
  std::map<std::pair<std::string, int>, std::unique_ptr<Face>> m_faces;
  bool m_initialised = false;
  uint64_t m_frame = 0;
};

/** The codepoints of UTF-8 text, one at a time (bad bytes come out as U+FFFD).
 */
template <typename Visit>
void forEachCodepoint(std::string_view text, Visit&& visit)
{
  for (std::size_t i = 0; i < text.size();)
  {
    const auto byte = static_cast<unsigned char>(text[i]);
    int length = byte < 0x80           ? 1
                 : (byte >> 5) == 0x6  ? 2
                 : (byte >> 4) == 0xE  ? 3
                 : (byte >> 3) == 0x1E ? 4
                                       : 0;
    char32_t codepoint = 0xFFFD;
    if (length > 0 && i + length <= text.size())
    {
      codepoint = length == 1 ? byte : byte & (0x7F >> length);
      for (int k = 1; k < length; ++k)
        codepoint =
            (codepoint << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    }
    else
      length = 1;
    visit(codepoint);
    i += static_cast<std::size_t>(length);
  }
}

} // namespace sfs::ui
