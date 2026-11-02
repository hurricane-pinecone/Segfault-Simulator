#include "engine/runtime/assetStore/textureImporter.h"

#include "engine/core/logger/logger.h"

#include <SDL_image.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace sfs
{

TextureImportSettings
TextureImportSettings::fromJson(const nlohmann::json& settings)
{
  TextureImportSettings result;
  if (settings.is_object() && settings.contains("filter") &&
      settings["filter"].is_string())
    result.nearest = settings["filter"].get<std::string>() == "nearest";
  if (settings.is_object() && settings.contains("spriteCell"))
  {
    const nlohmann::json& cell = settings["spriteCell"];
    if (cell.is_array() && cell.size() == 2 && cell[0].is_number_integer() &&
        cell[1].is_number_integer())
      result.spriteCell = {cell[0].get<int>(), cell[1].get<int>()};
  }
  if (settings.is_object() && settings.contains("pixelsPerUnit") &&
      settings["pixelsPerUnit"].is_number())
    result.pixelsPerUnit =
        std::max(settings["pixelsPerUnit"].get<float>(), 0.0f);
  return result;
}

std::shared_ptr<TextureData>
decodeTexture(const void* bytes, std::size_t size, const std::string& what)
{
  SDL_RWops* stream = SDL_RWFromConstMem(bytes, static_cast<int>(size));
  SDL_Surface* decoded = stream ? IMG_Load_RW(stream, 1) : nullptr;
  SDL_Surface* rgba =
      decoded ? SDL_ConvertSurfaceFormat(decoded, SDL_PIXELFORMAT_RGBA32, 0)
              : nullptr;
  SDL_FreeSurface(decoded);
  if (!rgba)
  {
    LOG_ERROR("Can't decode the image " + what + ": " + IMG_GetError());
    return nullptr;
  }

  std::shared_ptr<TextureData> texture = textureFromSurface(*rgba);
  SDL_FreeSurface(rgba);
  return texture;
}

std::shared_ptr<TextureData> textureFromSurface(const SDL_Surface& surface)
{
  SDL_Surface* rgba = SDL_ConvertSurfaceFormat(
      const_cast<SDL_Surface*>(&surface), SDL_PIXELFORMAT_RGBA32, 0);
  if (!rgba)
    return nullptr;
  auto texture = std::make_shared<TextureData>();
  texture->width = static_cast<uint32_t>(rgba->w);
  texture->height = static_cast<uint32_t>(rgba->h);
  const std::size_t row = std::size_t(texture->width) * 4;
  texture->rgba.resize(row * texture->height);
  for (uint32_t y = 0; y < texture->height; ++y)
    std::memcpy(texture->rgba.data() + y * row,
                static_cast<const uint8_t*>(rgba->pixels) + y * rgba->pitch,
                row);
  SDL_FreeSurface(rgba);
  return texture;
}

bool saveTexturePng(const TextureData& texture,
                    const std::filesystem::path& file)
{
  if (texture.empty() ||
      texture.rgba.size() < std::size_t(texture.width) * texture.height * 4)
    return false;
  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
      const_cast<uint8_t*>(texture.rgba.data()),
      static_cast<int>(texture.width),
      static_cast<int>(texture.height),
      32,
      static_cast<int>(texture.width * 4),
      SDL_PIXELFORMAT_RGBA32);
  const bool saved =
      surface && IMG_SavePNG(surface, file.string().c_str()) == 0;
  SDL_FreeSurface(surface);
  if (!saved)
    LOG_ERROR("Can't save the image " + file.string() + ": " + IMG_GetError());
  return saved;
}

std::shared_ptr<const TextureData>
importTexture(const std::filesystem::path& file,
              const TextureImportSettings& settings)
{
  std::ifstream in(file, std::ios::binary);
  const std::vector<char> bytes{
      std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (bytes.empty())
  {
    LOG_ERROR("Can't read the image " + file.string());
    return nullptr;
  }
  std::shared_ptr<TextureData> texture =
      decodeTexture(bytes.data(), bytes.size(), file.string());
  if (texture)
    texture->nearest = settings.nearest;
  return texture;
}

} // namespace sfs
