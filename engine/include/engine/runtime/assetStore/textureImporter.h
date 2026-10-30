#pragma once

#include "engine/core/rendering/mesh.h"
#include "glm/glm/ext/vector_int2.hpp"
#include "nlohmann/json.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

struct SDL_Surface;

namespace sfs
{

/** How an image file is brought in as a texture; kept in its asset's .meta
 * "settings". */
struct TextureImportSettings
{
  bool nearest =
      false; // "filter": "nearest" draws texels with hard edges (pixel art)
  // "spriteCell": [w, h] -- as a sprite sheet, a grid of frames this many
  // pixels each, numbered across then down from the top left; 0: the whole
  // image is one sprite.
  glm::ivec2 spriteCell{0, 0};

  static TextureImportSettings fromJson(const nlohmann::json& settings);
};

/**
 * Decode an image (png, jpg, bmp, tga, ...) in memory into RGBA texels.
 *
 * @param what names the image in the log if it can't be decoded
 * @return null (logged) if it can't
 */
std::shared_ptr<TextureData>
decodeTexture(const void* bytes, std::size_t size, const std::string& what);

/** An SDL surface's pixels as RGBA texels. Null if it can't be converted. */
std::shared_ptr<TextureData> textureFromSurface(const SDL_Surface& surface);

/** Save a texture's texels as a PNG file. False (logged) if it can't. */
bool saveTexturePng(const TextureData& texture,
                    const std::filesystem::path& file);

/** Read an image file as a texture. Null (logged) if it can't be read. */
std::shared_ptr<const TextureData>
importTexture(const std::filesystem::path& file,
              const TextureImportSettings& settings = {});

} // namespace sfs
