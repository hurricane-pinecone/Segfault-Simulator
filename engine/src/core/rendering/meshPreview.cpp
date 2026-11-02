#include "engine/core/rendering/meshPreview.h"

#include "engine/core/rendering/materialGraphCompiler.h"
#include "engine/core/rendering/materialNodes.h"
#include "engine/core/rendering/terrain.h"

#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace sfs::meshPreview
{

namespace
{
constexpr int kSupersample = 2;
constexpr float kPi = 3.14159265f;

float toLinear(uint8_t value)
{
  const float c = value / 255.0f;
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

uint8_t toSrgb(float linear)
{
  const float c = std::clamp(linear, 0.0f, 1.0f);
  const float s =
      c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
  return static_cast<uint8_t>(std::lround(s * 255.0f));
}

// A texel (repeating), as linear colour and alpha (or as stored, for data).
glm::vec4 sample(const sfs::TextureData& texture, glm::vec2 uv)
{
  uv -= glm::floor(uv);
  const auto x =
      std::min(static_cast<uint32_t>(uv.x * texture.width), texture.width - 1);
  const auto y = std::min(
      static_cast<uint32_t>(uv.y * texture.height), texture.height - 1);
  const uint8_t* texel =
      &texture.rgba[(std::size_t(y) * texture.width + x) * 4];
  if (texture.linear)
    return glm::vec4(texel[0], texel[1], texel[2], texel[3]) / 255.0f;
  return {toLinear(texel[0]),
          toLinear(texel[1]),
          toLinear(texel[2]),
          texel[3] / 255.0f};
}

bool usable(const std::shared_ptr<const sfs::TextureData>& texture)
{
  return texture && !texture->empty() &&
         texture->rgba.size() >=
             std::size_t(texture->width) * texture->height * 4;
}

// A material's surface at a point, as the GPU's surface() works it out: the
// standard material's, or its graph's.
sfs::materialGraph::SurfaceValues
surfaceAt(const sfs::MeshMaterial& material,
          const sfs::materialGraph::SurfacePoint& point)
{
  if (material.terrain)
  {
    // A terrain: its layers by the splat map at the point (each texture
    // tiling over the preview's UVs, about twice), without their heights.
    const sfs::TerrainMaterial& terrain = *material.terrain;
    const std::array<float, sfs::kMaxTerrainLayers> weights =
        sfs::terrainWeights(usable(terrain.splat)
                                ? sfs::sampleSplat(*terrain.splat, point.uv)
                                : glm::vec4(0.0f));
    sfs::materialGraph::SurfaceValues surface;
    surface.baseColor = glm::vec3(0.0f);
    surface.roughness = 0.0f;
    for (std::size_t i = 0; i < weights.size() && !terrain.layers.empty(); ++i)
    {
      const sfs::TerrainLayer& layer =
          terrain.layers[i < terrain.layers.size() ? i : 0];
      glm::vec3 color = layer.tint;
      if (usable(layer.baseTexture))
        color *= glm::vec3(sample(*layer.baseTexture, point.uv * 2.0f));
      surface.baseColor += color * weights[i];
      surface.roughness += layer.roughness * weights[i];
    }
    if (terrain.layers.empty())
      surface.baseColor = glm::vec3(1.0f);
    return surface;
  }
  if (material.shader)
  {
    // Textures the preview can't read (still loading) read white.
    std::vector<std::shared_ptr<const sfs::TextureData>> textures;
    for (const auto& texture : material.shaderTextures)
      textures.push_back(usable(texture) ? texture : nullptr);
    return material.shader->evaluate(
        point, material.shaderParameters, textures);
  }

  const glm::vec2 uv = point.uv * material.tiling + material.offset;
  glm::vec4 albedo = glm::vec4(point.vertexColor, 1.0f) * material.baseColor;
  if (usable(material.baseTexture))
    albedo *= sample(*material.baseTexture, uv);
  sfs::materialGraph::SurfaceValues surface;
  surface.baseColor = glm::vec3(albedo);
  surface.alpha = albedo.a;
  surface.metallic = material.metallic;
  surface.roughness = material.roughness;
  if (usable(material.metallicRoughnessTexture))
  {
    const glm::vec4 data = sample(*material.metallicRoughnessTexture, uv);
    surface.metallic *= data.b;
    surface.roughness *= data.g;
  }
  if (usable(material.normalTexture))
  {
    const glm::vec3 bump =
        glm::vec3(sample(*material.normalTexture, uv)) * 2.0f - 1.0f;
    surface.normal = glm::normalize(glm::vec3(
        glm::vec2(bump) * material.normalScale, std::max(bump.z, 0.0001f)));
  }
  surface.alphaClip = material.alphaCutoff;
  return surface;
}

// What shiny surfaces reflect: sky above (pale at the horizon), ground below.
glm::vec3 environment(const glm::vec3& direction, float roughness)
{
  const glm::vec3 sky(0.50f, 0.62f, 0.75f);
  const glm::vec3 horizon(0.86f, 0.88f, 0.85f);
  const glm::vec3 ground(0.22f, 0.21f, 0.19f);
  const float softness = glm::mix(0.05f, 0.6f, roughness);
  const glm::vec3 above =
      glm::mix(horizon, sky, glm::smoothstep(0.0f, 0.7f, direction.z));
  return glm::mix(
      ground, above, glm::smoothstep(-softness, softness * 0.5f, direction.z));
}

// One surface point lit: linear colour.
glm::vec3 shade(const glm::vec3& albedo,
                float metallic,
                float roughness,
                glm::vec3 normal,
                const glm::vec3& toView)
{
  const glm::vec3 toSun = glm::normalize(glm::vec3(-0.45f, -0.55f, 0.70f));
  const float ndotl = std::max(glm::dot(normal, toSun), 0.0f);
  const float ndotv = std::max(glm::dot(normal, toView), 1.0e-3f);

  const float ambient = 0.38f + 0.12f * normal.z;
  const glm::vec3 diffuse =
      albedo * (1.0f - metallic) * (ambient + 0.72f * ndotl);

  const glm::vec3 f0 = glm::mix(glm::vec3(0.04f), albedo, metallic);
  const glm::vec3 fresnel =
      f0 + (glm::max(glm::vec3(1.0f - roughness), f0) - f0) *
               std::pow(1.0f - ndotv, 5.0f);
  const float reflectivity = glm::mix(1.0f - roughness, 1.0f, metallic);
  const glm::vec3 reflected = glm::reflect(-toView, normal);
  const glm::vec3 up =
      glm::normalize(glm::mix(reflected, normal, roughness * roughness));
  const glm::vec3 reflection =
      environment(up, roughness) * fresnel * reflectivity;

  // A sun glint (GGX), sharp when smooth.
  const glm::vec3 halfway = glm::normalize(toSun + toView);
  const float a = std::max(roughness * roughness, 0.02f);
  const float d = std::pow(std::max(glm::dot(normal, halfway), 0.0f), 2.0f) *
                      (a * a - 1.0f) +
                  1.0f;
  const float glint = std::min(a * a / (kPi * d * d) * ndotl * 0.25f, 6.0f);
  return diffuse + reflection + f0 * glint * reflectivity;
}

struct Pixel
{
  float depth = std::numeric_limits<float>::max();
  glm::vec3 color{0.0f};
  float alpha = 0.0f;
};
} // namespace

std::vector<uint8_t>
render(const sfs::MeshData& mesh, int size, const sfs::MeshMaterial* material)
{
  std::vector<uint8_t> out(std::size_t(size) * size * 4, 0);
  if (mesh.empty() || size <= 0)
    return out;

  // Seen from above and to the front-right, framed to fill the picture.
  const float yaw = glm::radians(-55.0f), pitch = glm::radians(28.0f);
  const glm::vec3 toView(std::cos(pitch) * std::cos(yaw),
                         std::cos(pitch) * std::sin(yaw),
                         std::sin(pitch));
  const glm::vec3 forward = -toView;
  const glm::vec3 right =
      glm::normalize(glm::cross(forward, glm::vec3(0, 0, 1)));
  const glm::vec3 up = glm::cross(right, forward);

  glm::vec2 low(std::numeric_limits<float>::max()),
      high(std::numeric_limits<float>::lowest());
  for (const sfs::MeshVertex& vertex : mesh.vertices)
  {
    const glm::vec2 flat(
        glm::dot(vertex.position, right), glm::dot(vertex.position, up));
    low = glm::min(low, flat);
    high = glm::max(high, flat);
  }
  const glm::vec2 middle = (low + high) * 0.5f;
  const float span =
      std::max(std::max(high.x - low.x, high.y - low.y), 1.0e-4f);
  const glm::vec3 centre = right * middle.x + up * middle.y;

  const int n = size * kSupersample;
  const float scale = n * 0.86f / span;
  std::vector<Pixel> pixels(std::size_t(n) * n);

  struct Projected
  {
    glm::vec2 screen;
    float depth;
  };
  std::vector<Projected> projected(mesh.vertices.size());
  for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
  {
    const glm::vec3 p = mesh.vertices[i].position - centre;
    projected[i] = {{n * 0.5f + glm::dot(p, right) * scale,
                     n * 0.5f - glm::dot(p, up) * scale},
                    glm::dot(p, forward)};
  }

  const sfs::MeshMaterial defaults;
  const auto materialOf = [&](std::size_t index) -> const sfs::MeshMaterial&
  {
    if (material)
      return *material;
    for (const sfs::MeshSection& section : mesh.sections)
      if (index >= section.firstIndex &&
          index < std::size_t(section.firstIndex) + section.indexCount)
        return section.material;
    return defaults;
  };

  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
  {
    const uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1],
                   ic = mesh.indices[i + 2];
    if (ia >= projected.size() || ib >= projected.size() ||
        ic >= projected.size())
      continue;
    const Projected &a = projected[ia], &b = projected[ib], &c = projected[ic];
    const float area = (b.screen.x - a.screen.x) * (c.screen.y - a.screen.y) -
                       (b.screen.y - a.screen.y) * (c.screen.x - a.screen.x);
    if (std::abs(area) < 1.0e-8f)
      continue;
    const sfs::MeshMaterial& material = materialOf(i);
    // Which way the texture runs across the triangle, for normal maps.
    glm::vec3 alongU, upImage;
    {
      const glm::vec3 positions[3] = {mesh.vertices[ia].position,
                                      mesh.vertices[ib].position,
                                      mesh.vertices[ic].position};
      const glm::vec2 uvs[3] = {
          mesh.vertices[ia].uv, mesh.vertices[ib].uv, mesh.vertices[ic].uv};
      sfs::materialGraph::textureDirections(positions, uvs, alongU, upImage);
    }

    const int minX = std::max(0,
                              static_cast<int>(std::floor(std::min(
                                  {a.screen.x, b.screen.x, c.screen.x}))));
    const int maxX = std::min(n - 1,
                              static_cast<int>(std::ceil(std::max(
                                  {a.screen.x, b.screen.x, c.screen.x}))));
    const int minY = std::max(0,
                              static_cast<int>(std::floor(std::min(
                                  {a.screen.y, b.screen.y, c.screen.y}))));
    const int maxY = std::min(n - 1,
                              static_cast<int>(std::ceil(std::max(
                                  {a.screen.y, b.screen.y, c.screen.y}))));
    for (int y = minY; y <= maxY; ++y)
      for (int x = minX; x <= maxX; ++x)
      {
        const glm::vec2 p(x + 0.5f, y + 0.5f);
        const float w0 = ((b.screen.x - p.x) * (c.screen.y - p.y) -
                          (b.screen.y - p.y) * (c.screen.x - p.x)) /
                         area;
        const float w1 = ((c.screen.x - p.x) * (a.screen.y - p.y) -
                          (c.screen.y - p.y) * (a.screen.x - p.x)) /
                         area;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
          continue;
        const float depth = w0 * a.depth + w1 * b.depth + w2 * c.depth;
        Pixel& pixel = pixels[std::size_t(y) * n + x];
        if (depth >= pixel.depth)
          continue;

        const sfs::MeshVertex &va = mesh.vertices[ia], &vb = mesh.vertices[ib],
                              &vc = mesh.vertices[ic];
        glm::vec3 normal = w0 * va.normal + w1 * vb.normal + w2 * vc.normal;
        const float length = glm::length(normal);
        normal = length > 0.0f ? normal / length : toView;
        if (glm::dot(normal, toView) < 0.0f)
          normal = -normal; // either side of an open mesh

        sfs::materialGraph::SurfacePoint point;
        point.uv = w0 * va.uv + w1 * vb.uv + w2 * vc.uv;
        point.position = w0 * va.position + w1 * vb.position + w2 * vc.position;
        point.normal = normal;
        sfs::materialGraph::tangentFrame(
            normal, alongU, upImage, point.tangent, point.bitangent);
        point.toView = toView;
        point.vertexColor = w0 * va.color + w1 * vb.color + w2 * vc.color;
        const sfs::materialGraph::SurfaceValues surface =
            surfaceAt(material, point);
        const glm::vec3 shadingNormal =
            sfs::materialGraph::perturbNormal(point, surface.normal);

        float alpha =
            material.alphaMode == sfs::AlphaMode::Opaque ? 1.0f : surface.alpha;
        if (material.alphaMode == sfs::AlphaMode::Mask)
        {
          if (alpha < surface.alphaClip)
            continue;
          alpha = 1.0f;
        }
        else if (sfs::isSeeThrough(material.alphaMode) && alpha < 0.02f)
          continue;

        pixel.depth = depth;
        pixel.color = shade(surface.baseColor,
                            std::clamp(surface.metallic, 0.0f, 1.0f),
                            std::clamp(surface.roughness, 0.04f, 1.0f),
                            shadingNormal,
                            toView) +
                      glm::max(surface.emission, glm::vec3(0.0f));
        pixel.alpha = alpha;
      }
  }

  // Average each supersampled block, weighting colour by coverage.
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
    {
      glm::vec3 color(0.0f);
      float alpha = 0.0f;
      for (int sy = 0; sy < kSupersample; ++sy)
        for (int sx = 0; sx < kSupersample; ++sx)
        {
          const Pixel& pixel = pixels[std::size_t(y * kSupersample + sy) * n +
                                      x * kSupersample + sx];
          color += pixel.color * pixel.alpha;
          alpha += pixel.alpha;
        }
      uint8_t* texel = &out[(std::size_t(y) * size + x) * 4];
      if (alpha > 0.0f)
      {
        color /= alpha;
        texel[0] = toSrgb(color.r);
        texel[1] = toSrgb(color.g);
        texel[2] = toSrgb(color.b);
      }
      texel[3] = static_cast<uint8_t>(
          std::lround(alpha / (kSupersample * kSupersample) * 255.0f));
    }
  return out;
}

std::vector<uint8_t> renderMaterial(const sfs::MeshMaterial& material, int size)
{
  // A UV sphere, u going twice around so square textures stay square.
  static const sfs::MeshData sphere = []
  {
    sfs::MeshData mesh;
    constexpr int segments = 48, rings = 24;
    for (int r = 0; r <= rings; ++r)
    {
      const float theta = kPi * r / rings;
      for (int s = 0; s <= segments; ++s)
      {
        const float phi = 2.0f * kPi * s / segments;
        const glm::vec3 normal(std::sin(theta) * std::cos(phi),
                               std::sin(theta) * std::sin(phi),
                               std::cos(theta));
        mesh.vertices.push_back(
            {normal * 0.5f,
             normal,
             glm::vec3(1.0f),
             glm::vec2(2.0f * s / segments, static_cast<float>(r) / rings)});
      }
    }
    for (int r = 0; r < rings; ++r)
      for (int s = 0; s < segments; ++s)
      {
        const uint32_t a = r * (segments + 1) + s, b = a + segments + 1;
        mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
      }
    return mesh;
  }();
  return render(sphere, size, &material);
}

std::vector<uint8_t> fitTexture(const sfs::TextureData& texture, int size)
{
  std::vector<uint8_t> out(std::size_t(size) * size * 4, 0);
  if (texture.empty() || size <= 0 ||
      texture.rgba.size() < std::size_t(texture.width) * texture.height * 4)
    return out;

  // The texels each output pixel covers, averaged (nearest when enlarging).
  const float scale = std::min(static_cast<float>(size) / texture.width,
                               static_cast<float>(size) / texture.height);
  const int width = std::max(1, static_cast<int>(texture.width * scale));
  const int height = std::max(1, static_cast<int>(texture.height * scale));
  const int left = (size - width) / 2, top = (size - height) / 2;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
    {
      const uint32_t x0 = static_cast<uint32_t>(x / scale),
                     y0 = static_cast<uint32_t>(y / scale);
      const uint32_t x1 = std::max(
          x0 + 1,
          std::min(texture.width, static_cast<uint32_t>((x + 1) / scale)));
      const uint32_t y1 = std::max(
          y0 + 1,
          std::min(texture.height, static_cast<uint32_t>((y + 1) / scale)));
      std::array<uint32_t, 4> sum{};
      uint32_t count = 0;
      for (uint32_t ty = y0; ty < y1 && ty < texture.height; ++ty)
        for (uint32_t tx = x0; tx < x1 && tx < texture.width; ++tx, ++count)
          for (int c = 0; c < 4; ++c)
            sum[c] +=
                texture.rgba[(std::size_t(ty) * texture.width + tx) * 4 + c];
      uint8_t* texel = &out[(std::size_t(top + y) * size + left + x) * 4];
      for (int c = 0; c < 4; ++c)
        texel[c] = static_cast<uint8_t>(count ? sum[c] / count : 0);
    }
  return out;
}

} // namespace sfs::meshPreview
