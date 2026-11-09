#pragma once

#include "engine/runtime/rendering/uiDraw.h"
#include "engine/runtime/ui/layout.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sfs::ui
{

/**
 * Where the UI's images live: small ones (icons, up to kLargest pixels a
 * side) packed on shared pages, so a row of icons and text draws in one go,
 * each with its smaller levels (half, a quarter... averaged) beside it, so
 * it's drawn crisp at any size; bigger ones in a texture of their own. What's
 * on a page never moves, so a layout that used it stays right.
 */
class ImageAtlas
{
public:
  static constexpr int kPageSize = 1024;
  static constexpr int kLargest = 256;

  /**
   * `pixels` (RGBA8, rows from the top, `size.x * size.y * 4` bytes) added:
   * where it went, kept while the atlas lives; null if it's no size.
   */
  const SourceImage* add(const std::vector<uint8_t>& pixels, glm::ivec2 size);

  /** How many shared pages there are. */
  std::size_t pages() const { return m_pages.size(); }

private:
  struct Page
  {
    UiTexture texture;
    int x = 0, y = 0; // where the next goes on the shelf
    int shelf = 0;    // the shelf's height
  };

  std::vector<std::unique_ptr<Page>> m_pages;
  std::vector<std::unique_ptr<UiTexture>> m_own; // big images'
  std::vector<std::unique_ptr<SourceImage>> m_images;
};

/**
 * `pixels` (RGBA8, `size`) half the size (rounded up), each pixel the
 * average of the 2 x 2 under it, colours weighted by their alpha (so a clear
 * edge doesn't darken an icon).
 */
std::vector<uint8_t> halved(const std::vector<uint8_t>& pixels,
                            glm::ivec2 size);

} // namespace sfs::ui
