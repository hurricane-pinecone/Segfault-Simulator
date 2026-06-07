#pragma once

#include "engine/core/Color/Color.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/iQuadRenderer.h"
#include <string>
#include <unordered_map>

namespace sfs
{

struct CachedText
{
  int width = 0;
  int height = 0;
  unsigned int texture = 0; // GL texture handle
};

class TextRenderer
{
public:
  TextRenderer(IQuadRenderer& quadRenderer, AssetStore& assetStore);
  ~TextRenderer();

  bool init();

  bool isInitialized() const;

  void drawText(float x, float y, const std::string& text);

  void drawText(float x, float y, const std::string& text, Color color);

  void drawText(float x,
                float y,
                const std::string& text,
                const std::string& fontId,
                Color color = Colors::White);

  // Line spacing in pixels for a loaded font, for stacking multiple lines.
  int lineHeight(const std::string& fontId) const;

private:
  static std::string buildCacheKey(const std::string& text,
                                   const std::string& fontId,
                                   Color color);

  IQuadRenderer& m_quadRenderer;
  AssetStore& m_assetStore;

  bool m_initialized = false;
  std::unordered_map<std::string, CachedText> m_textCache;
};

} // namespace sfs
