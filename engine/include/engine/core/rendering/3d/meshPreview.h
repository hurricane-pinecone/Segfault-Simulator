#pragma once

#include "engine/core/rendering/3d/mesh.h"

#include <cstdint>
#include <vector>

// A tiny software renderer for previews and icons (the editor's Assets panel,
// a game UI's ui::Image::mesh): a mesh drawn from a three-quarter view, lit
// like the editor's scenes, on a transparent background. CPU only, so they
// need no GPU passes of their own.
namespace sfs::meshPreview
{

/**
 * Draw a mesh into a size x size image: RGBA, sRGB, top row first, alpha 0
 * where nothing was drawn. Each section is drawn with its own material, or
 * with `material` for all of them if given.
 */
std::vector<uint8_t> render(const sfs::MeshData& mesh,
                            int size,
                            const sfs::MeshMaterial* material = nullptr);

/** A material on a sphere (UV-mapped, so its textures show). */
std::vector<uint8_t> renderMaterial(const sfs::MeshMaterial& material,
                                    int size);

/**
 * A texture scaled down to fit size x size (keeping its shape, centred), as
 * RGBA sRGB. Transparent texels stay transparent.
 */
std::vector<uint8_t> fitTexture(const sfs::TextureData& texture, int size);

} // namespace sfs::meshPreview
