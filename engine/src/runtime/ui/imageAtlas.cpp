#include "engine/runtime/ui/imageAtlas.h"

#include <algorithm>
#include <cstring>

namespace sfs::ui
{

namespace
{
constexpr int kGutter = 2;   // clear pixels between images on a page
constexpr int kSmallest = 8; // levels stop before they're smaller than this
} // namespace

std::vector<uint8_t> halved(const std::vector<uint8_t>& pixels, glm::ivec2 size)
{
  const glm::ivec2 half = glm::max((size + 1) / 2, glm::ivec2(1));
  std::vector<uint8_t> out(std::size_t(half.x) * half.y * 4);
  for (int y = 0; y < half.y; ++y)
    for (int x = 0; x < half.x; ++x)
    {
      float colour[3] = {0.0f, 0.0f, 0.0f};
      float plain[3] = {0.0f, 0.0f, 0.0f};
      float alpha = 0.0f;
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
          const int sx = std::min(2 * x + dx, size.x - 1);
          const int sy = std::min(2 * y + dy, size.y - 1);
          const uint8_t* p = &pixels[(std::size_t(sy) * size.x + sx) * 4];
          const float a = p[3] / 255.0f;
          for (int c = 0; c < 3; ++c)
          {
            colour[c] += p[c] * a;
            plain[c] += p[c];
          }
          alpha += a;
        }
      uint8_t* q = &out[(std::size_t(y) * half.x + x) * 4];
      for (int c = 0; c < 3; ++c)
        q[c] = static_cast<uint8_t>(std::clamp(
            alpha > 0.0f ? colour[c] / alpha + 0.5f : plain[c] / 4.0f + 0.5f,
            0.0f,
            255.0f));
      q[3] = static_cast<uint8_t>(
          std::clamp(alpha / 4.0f * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  return out;
}

const SourceImage* ImageAtlas::add(const std::vector<uint8_t>& pixels,
                                   glm::ivec2 size)
{
  if (size.x <= 0 || size.y <= 0 ||
      pixels.size() < std::size_t(size.x) * size.y * 4)
    return nullptr;
  auto image = std::make_unique<SourceImage>();

  // Big: a texture of its own, one level.
  if (size.x > kLargest || size.y > kLargest)
  {
    auto texture = std::make_unique<UiTexture>();
    texture->id = nextUiTextureId();
    texture->size = size;
    texture->pixels.assign(
        pixels.begin(), pixels.begin() + std::size_t(size.x) * size.y * 4);
    texture->revision = 1;
    image->texture = texture.get();
    image->level[0] = {glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), size};
    m_own.push_back(std::move(texture));
    m_images.push_back(std::move(image));
    return m_images.back().get();
  }

  // Its levels: itself, then each half the last, down to the smallest.
  std::vector<std::vector<uint8_t>> levels{std::vector<uint8_t>(
      pixels.begin(), pixels.begin() + std::size_t(size.x) * size.y * 4)};
  std::vector<glm::ivec2> sizes{size};
  while (static_cast<int>(levels.size()) < SourceImage::kMostLevels &&
         std::min(sizes.back().x, sizes.back().y) / 2 >= kSmallest)
  {
    levels.push_back(halved(levels.back(), sizes.back()));
    sizes.push_back(glm::max((sizes.back() + 1) / 2, glm::ivec2(1)));
  }

  // Its cell: itself, and its levels down a column beside it.
  int column = 0, columnWidth = 0;
  for (std::size_t i = 1; i < sizes.size(); ++i)
  {
    column += sizes[i].y + kGutter;
    columnWidth = std::max(columnWidth, sizes[i].x);
  }
  const glm::ivec2 cell{size.x + kGutter + columnWidth + kGutter,
                        std::max(size.y, column) + kGutter};

  // Where it goes: along the last page's shelf, or a new shelf, or page.
  Page* page = m_pages.empty() ? nullptr : m_pages.back().get();
  if (page && page->x + cell.x > kPageSize)
  {
    page->y += page->shelf;
    page->x = 0;
    page->shelf = 0;
  }
  if (!page || page->y + cell.y > kPageSize)
  {
    auto made = std::make_unique<Page>();
    made->texture.id = nextUiTextureId();
    made->texture.size = {kPageSize, kPageSize};
    made->texture.pixels.assign(std::size_t(kPageSize) * kPageSize * 4, 0);
    m_pages.push_back(std::move(made));
    page = m_pages.back().get();
  }

  // Each level copied there.
  glm::ivec2 at{page->x, page->y};
  for (std::size_t i = 0; i < levels.size(); ++i)
  {
    for (int y = 0; y < sizes[i].y; ++y)
      std::memcpy(
          &page->texture.pixels[(std::size_t(at.y + y) * kPageSize + at.x) * 4],
          &levels[i][std::size_t(y) * sizes[i].x * 4],
          std::size_t(sizes[i].x) * 4);
    image->level[i] = {glm::vec4(glm::vec2(at), glm::vec2(sizes[i])) /
                           static_cast<float>(kPageSize),
                       sizes[i]};
    if (i == 0)
      at = {page->x + size.x + kGutter, page->y};
    else
      at.y += sizes[i].y + kGutter;
  }
  image->levels = static_cast<int>(levels.size());
  image->texture = &page->texture;
  ++page->texture.revision;
  page->x += cell.x;
  page->shelf = std::max(page->shelf, cell.y);
  m_images.push_back(std::move(image));
  return m_images.back().get();
}

} // namespace sfs::ui
