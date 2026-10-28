#pragma once

#include "engine/core/rendering/3d/mesh.h"
#include "nlohmann/json.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace sfs
{

/** How an image file is brought in as a texture; kept in its asset's .meta "settings". */
struct TextureImportSettings
{
  bool nearest = false; // "filter": "nearest" draws texels with hard edges (pixel art)

  static TextureImportSettings fromJson(const nlohmann::json& settings);
};

/**
 * Decode an image (png, jpg, bmp, tga, ...) in memory into RGBA texels.
 *
 * @param what names the image in the log if it can't be decoded
 * @return null (logged) if it can't
 */
std::shared_ptr<TextureData> decodeTexture(const void* bytes, std::size_t size, const std::string& what);

/** Save a texture's texels as a PNG file. False (logged) if it can't. */
bool saveTexturePng(const TextureData& texture, const std::filesystem::path& file);

/** Read an image file as a texture. Null (logged) if it can't be read. */
std::shared_ptr<const TextureData> importTexture(const std::filesystem::path& file,
                                                 const TextureImportSettings& settings = {});

} // namespace sfs
