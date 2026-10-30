#include "engine/runtime/ui/fontAtlas.h"

#include "engine/core/logger/logger.h"

#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sfs::ui
{

namespace
{
constexpr int kStartSize = 256; // a face's texture to begin with, a side
constexpr int kMostSize = 4096;
uint32_t g_nextTextureId = 0x55490000u; // "UI": apart from other textures
} // namespace

FontAtlas::FontAtlas(std::function<std::string(const std::string&)> resolve,
                     std::string fallbackFile)
    : m_resolve(std::move(resolve)), m_fallback(std::move(fallbackFile))
{
  if (!TTF_WasInit() && TTF_Init() != 0)
    LOG_ERROR(std::string("UI text can't start (SDL_ttf): ") + TTF_GetError());
  else
    m_initialised = true;
}

FontAtlas::~FontAtlas()
{
  for (auto& [key, face] : m_faces)
    if (face && face->font)
      TTF_CloseFont(face->font);
}

FontAtlas::Face* FontAtlas::face(const std::string& font, float pixels)
{
  const int size = std::max(1, static_cast<int>(std::lround(pixels)));
  auto [it, added] = m_faces.try_emplace({font, size});
  if (!added)
    return it->second.get();

  auto made = std::make_unique<Face>();
  std::string path = m_initialised ? m_resolve(font) : std::string();
  if (!path.empty())
    made->font = TTF_OpenFont(path.c_str(), size);
  if (!made->font && m_initialised)
  {
    if (font != m_fallback)
      LOG_ERROR(
          "UI font '" + font + "' can't be opened" +
          (path.empty() ? std::string() : std::string(": ") + TTF_GetError()) +
          "; using " + m_fallback);
    const std::string fallback = m_resolve(m_fallback);
    if (!fallback.empty())
      made->font = TTF_OpenFont(fallback.c_str(), size);
  }
  if (made->font)
  {
    TTF_SetFontHinting(made->font, TTF_HINTING_LIGHT);
    made->lineHeight = static_cast<float>(TTF_FontLineSkip(made->font));
  }
  else
    made->lineHeight = static_cast<float>(size);
  made->texture.id = ++g_nextTextureId;
  made->texture.size = {kStartSize, kStartSize};
  made->texture.pixels.assign(std::size_t(kStartSize) * kStartSize * 4, 0);
  it->second = std::move(made);
  return it->second.get();
}

const FontAtlas::Glyph& FontAtlas::glyph(Face& face, char32_t codepoint)
{
  if (const auto it = face.glyphs.find(codepoint); it != face.glyphs.end())
    return it->second;
  Glyph& glyph = face.glyphs[codepoint];
  if (!face.font)
    return glyph;

  int minX = 0, maxX = 0, minY = 0, maxY = 0, advance = 0;
  if (TTF_GlyphMetrics32(face.font,
                         static_cast<Uint32>(codepoint),
                         &minX,
                         &maxX,
                         &minY,
                         &maxY,
                         &advance) != 0)
    return glyph; // not in the font
  glyph.advance = static_cast<float>(advance);
  if (codepoint == U' ' || codepoint == U'\t')
    return glyph;

  // Rendered white, its coverage in alpha, a box the font's height.
  SDL_Surface* rendered = TTF_RenderGlyph32_Blended(
      face.font, static_cast<Uint32>(codepoint), SDL_Color{255, 255, 255, 255});
  if (!rendered)
    return glyph;
  SDL_Surface* rgba =
      SDL_ConvertSurfaceFormat(rendered, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(rendered);
  if (!rgba)
    return glyph;

  // Onto the next shelf with room (a pixel apart, so neighbours don't bleed),
  // the texture made taller when it's full.
  UiTexture& texture = face.texture;
  if (face.shelf.x + rgba->w + 1 > texture.size.x)
  {
    face.shelf = {1, face.shelf.y + face.shelfHeight + 1};
    face.shelfHeight = 0;
  }
  while (face.shelf.y + rgba->h + 1 > texture.size.y &&
         texture.size.y < kMostSize)
  {
    texture.size.y *= 2;
    texture.pixels.resize(std::size_t(texture.size.x) * texture.size.y * 4, 0);
  }
  if (face.shelf.y + rgba->h + 1 <= texture.size.y &&
      rgba->w + 2 <= texture.size.x)
  {
    SDL_LockSurface(rgba);
    for (int y = 0; y < rgba->h; ++y)
      std::memcpy(
          &texture.pixels[(std::size_t(face.shelf.y + y) * texture.size.x +
                           face.shelf.x) *
                          4],
          static_cast<const uint8_t*>(rgba->pixels) +
              std::size_t(y) * rgba->pitch,
          std::size_t(rgba->w) * 4);
    SDL_UnlockSurface(rgba);
    glyph.at = face.shelf;
    glyph.size = {rgba->w, rgba->h};
    face.shelf.x += rgba->w + 1;
    face.shelfHeight = std::max(face.shelfHeight, rgba->h);
    ++texture.revision;
  }
  SDL_FreeSurface(rgba);
  return glyph;
}

float FontAtlas::lineHeight(const std::string& font, float pixels)
{
  return face(font, pixels)->lineHeight;
}

float FontAtlas::width(const std::string& font,
                       float pixels,
                       std::string_view line)
{
  Face& f = *face(font, pixels);
  float pen = 0.0f;
  char32_t previous = 0;
  forEachCodepoint(line,
                   [&](char32_t codepoint)
                   {
                     if (previous && f.font)
                       pen += static_cast<float>(TTF_GetFontKerningSizeGlyphs32(
                           f.font,
                           static_cast<Uint32>(previous),
                           static_cast<Uint32>(codepoint)));
                     pen += glyph(f, codepoint).advance;
                     previous = codepoint;
                   });
  return pen;
}

void FontAtlas::draw(const std::string& font,
                     float pixels,
                     std::string_view line,
                     glm::vec2 at,
                     glm::vec4 color,
                     std::vector<UiQuad>& out)
{
  Face& f = *face(font, pixels);
  float pen = at.x;
  char32_t previous = 0;
  forEachCodepoint(line,
                   [&](char32_t codepoint)
                   {
                     if (previous && f.font)
                       pen += static_cast<float>(TTF_GetFontKerningSizeGlyphs32(
                           f.font,
                           static_cast<Uint32>(previous),
                           static_cast<Uint32>(codepoint)));
                     previous = codepoint;
                     const Glyph& g = glyph(f, codepoint);
                     if (g.size.x > 0 && g.size.y > 0)
                     {
                       UiQuad quad;
                       quad.kind = UiQuad::Glyph;
                       quad.texture = &f.texture;
                       quad.rect = {std::round(pen),
                                    at.y,
                                    static_cast<float>(g.size.x),
                                    static_cast<float>(g.size.y)};
                       const glm::vec2 texel = 1.0f / glm::vec2(f.texture.size);
                       quad.uv = {g.at.x * texel.x,
                                  g.at.y * texel.y,
                                  g.size.x * texel.x,
                                  g.size.y * texel.y};
                       quad.color = color;
                       out.push_back(quad);
                     }
                     pen += g.advance;
                   });
}

} // namespace sfs::ui
