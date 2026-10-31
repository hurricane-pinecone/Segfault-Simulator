#include "../testHarness.h"

#include <engine/core/logger/logger.h>
#include <engine/core/rendering/materialGraphCompiler.h>
#include <engine/core/rendering/meshPrimitives.h>
#include <engine/core/rendering/sky.h>
#include <engine/core/rendering/sunShadow.h>
#include <engine/core/rendering/terrain.h>
#include <engine/webgpu/webGpuContext.h>
#include <engine/webgpu/webGpuRenderer.h>
#include <engine/webgpu/wgpuHandle.h>
#include <engine/webgpu/wgpuUtil.h>

#include <webgpu/wgpu.h> // wgpuDevicePoll

#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/gtc/constants.hpp"
#include "glm/glm/trigonometric.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <vector>

using namespace sfs;
namespace graph = sfs::materialGraph;

namespace
{

constexpr glm::ivec2 kSize{64, 64};

// Draws frames into an offscreen target, catching every error WebGPU (or the
// renderer) logs.
struct Harness
{
  WebGpuContext& context;
  WebGpuRenderer renderer;
  wgpu::Texture target;
  wgpu::TextureView view;
  MeshHandle mesh;

  explicit Harness(WebGpuContext& context)
      : context(context), renderer(context, 256)
  {
  }

  bool initialize()
  {
    if (!renderer.initialize())
      return false;
    WGPUTextureDescriptor desc = {};
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {
        static_cast<uint32_t>(kSize.x), static_cast<uint32_t>(kSize.y), 1};
    desc.format = context.format();
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    target.reset(wgpuDeviceCreateTexture(context.device(), &desc));
    view.reset(wgpuTextureCreateView(target.get(), nullptr));
    mesh = renderer.createMesh(meshes::sphere(1.0f, 12, 8));
    return target && view && mesh.valid();
  }

  // One frame of the sphere with `material`: a solid copy and a faded one
  // (the see-through pipeline), casting shadows.
  void draw(MaterialHandle material)
  {
    const WGPUCommandEncoder encoder =
        wgpuDeviceCreateCommandEncoder(context.device(), nullptr);
    renderer.setTarget(encoder, view.get(), kSize);

    FrameParams frame;
    frame.cameraPosition = {4.0f, 0.0f, 1.0f};
    frame.view = glm::lookAt(
        frame.cameraPosition, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    frame.projection =
        glm::perspectiveRH_ZO(1.0f, 1.0f, 0.1f, 100.0f); // WebGPU's 0..1 depth
    frame.viewProjection = frame.projection * frame.view;
    frame.shadows.enabled = true;
    frame.lightViewProjection =
        glm::orthoRH_ZO(-5.0f, 5.0f, -5.0f, 5.0f, 0.1f, 50.0f) *
        glm::lookAt(glm::vec3(0.0f, 0.0f, 20.0f),
                    glm::vec3(0.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f));
    frame.time = 1.5f;
    renderer.beginFrame(frame);

    MeshInstance solid;
    MeshInstance faded;
    faded.model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f));
    faded.tint.a = 0.5f;
    const MeshInstance instances[] = {solid, faded};
    const MaterialHandle materials[] = {material};
    MeshBatch batch;
    batch.mesh = mesh;
    batch.materials = materials;
    batch.instances = instances;
    batch.shadowInstances = instances;
    renderer.drawMesh(batch);
    renderer.endFrame();

    const WGPUCommandBuffer commands =
        wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(context.queue(), 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuDevicePoll(context.device(), true, nullptr);
  }
};

// The plane the parity test draws: 2 x 2 at Z = 0, filling an orthographic
// view from straight above, so pixel (x, y) shows UV ((x + 0.5) / size,
// (y + 0.5) / size).
constexpr glm::vec3 kParityCamera{0.0f, 0.0f, 5.0f};

// Draw `material` on the plane and read the pixels back (RGBA8, top row
// first).
std::vector<uint8_t>
drawPlane(Harness& harness,
          MeshHandle plane,
          MaterialHandle material,
          float time,
          const std::function<void(FrameParams&)>& changeFrame = {},
          const MeshInstance& instance = {},
          std::span<const MeshBatch> more = {},
          std::span<const GrassBatch> grass = {},
          std::span<const UiQuad> ui = {})
{
  WebGpuContext& context = harness.context;
  const WGPUCommandEncoder encoder =
      wgpuDeviceCreateCommandEncoder(context.device(), nullptr);
  harness.renderer.setTarget(encoder, harness.view.get(), kSize);

  FrameParams frame;
  frame.cameraPosition = kParityCamera;
  frame.view =
      glm::lookAt(kParityCamera, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  frame.projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
  frame.viewProjection = frame.projection * frame.view;
  frame.shadows.enabled = false;
  frame.time = time;
  frame.cameraForward = {0.0f, 0.0f, -1.0f};
  if (changeFrame)
    changeFrame(frame);
  harness.renderer.beginFrame(frame);
  const MaterialHandle materials[] = {material};
  MeshBatch batch;
  batch.mesh = plane;
  batch.materials = materials;
  batch.instances = {&instance, 1};
  harness.renderer.drawMesh(batch);
  for (const MeshBatch& other : more)
    harness.renderer.drawMesh(other);
  for (const GrassBatch& field : grass)
    harness.renderer.drawGrass(field);
  if (!ui.empty())
    harness.renderer.drawUi(
        UiDrawList{std::vector<UiQuad>(ui.begin(), ui.end())});
  harness.renderer.endFrame();

  // Rows padded to 256 bytes, as copies need.
  const uint32_t rowBytes = 256;
  wgpu::Buffer readback(
      wgpu::makeBuffer(context.device(),
                       uint64_t(rowBytes) * kSize.y,
                       WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead));
  WGPUTexelCopyTextureInfo source = {};
  source.texture = harness.target.get();
  source.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo destination = {};
  destination.buffer = readback.get();
  destination.layout.bytesPerRow = rowBytes;
  destination.layout.rowsPerImage = kSize.y;
  const WGPUExtent3D extent = {uint32_t(kSize.x), uint32_t(kSize.y), 1};
  wgpuCommandEncoderCopyTextureToBuffer(
      encoder, &source, &destination, &extent);
  const WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(context.queue(), 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);

  bool mapped = false;
  WGPUBufferMapCallbackInfo callback = {};
  callback.mode = WGPUCallbackMode_AllowProcessEvents;
  callback.callback =
      [](WGPUMapAsyncStatus status, WGPUStringView, void* done, void*)
  { *static_cast<bool*>(done) = status == WGPUMapAsyncStatus_Success; };
  callback.userdata1 = &mapped;
  wgpuBufferMapAsync(readback.get(),
                     WGPUMapMode_Read,
                     0,
                     uint64_t(rowBytes) * kSize.y,
                     callback);
  for (int i = 0; i < 100 && !mapped; ++i)
    wgpuDevicePoll(context.device(), true, nullptr);
  std::vector<uint8_t> pixels;
  if (!mapped)
    return pixels;
  const auto* data = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(
      readback.get(), 0, uint64_t(rowBytes) * kSize.y));
  for (int y = 0; y < kSize.y; ++y)
    pixels.insert(
        pixels.end(), data + y * rowBytes, data + y * rowBytes + kSize.x * 4);
  wgpuBufferUnmap(readback.get());
  return pixels;
}

MeshMaterial graphMaterial(const graph::Graph& g, AlphaMode mode)
{
  const graph::CompiledGraph compiled = graph::compile(g);
  MeshMaterial material;
  material.alphaMode = mode;
  material.shader = compiled.shader;
  material.shaderParameters = compiled.parameters;
  material.shaderTextures.resize(compiled.textures.size());
  return material;
}

} // namespace

int main()
{
  std::vector<std::string> errors;
  Logger::addSink(
      [&errors](Logger::Level level, const std::string& message)
      {
        if (level == Logger::Level::ERROR)
          errors.push_back(message);
      });
  const auto reportErrors = [&errors]
  {
    for (const std::string& error : errors)
      std::fprintf(stderr, "  logged: %s\n", error.c_str());
    const bool none = errors.empty();
    errors.clear();
    return none;
  };

  WebGpuContext context;
  if (!context.initHeadless())
  {
    std::fputs(
        "webGpuRendererTests: no WebGPU adapter here, skipped\n", stderr);
    return 77;
  }
  errors.clear();

  Harness harness(context);
  if (!harness.initialize())
  {
    // Nothing else can draw (logged why).
    reportErrors();
    CHECK(false);
    return testing::report("webGpuRendererTests");
  }
  TEST("the standard surface should build and draw")
  {
    MeshMaterial standard;
    standard.baseColor = {0.5f, 0.75f, 1.0f, 0.5f};
    auto bumps = std::make_shared<TextureData>();
    bumps->width = bumps->height = 1;
    bumps->rgba = {200, 128, 230, 255};
    bumps->linear = true;
    standard.normalTexture = bumps;
    for (const AlphaMode mode : {AlphaMode::Opaque,
                                 AlphaMode::Mask,
                                 AlphaMode::Blend,
                                 AlphaMode::Additive})
    {
      standard.alphaMode = mode;
      const MaterialHandle handle = harness.renderer.createMaterial(standard);
      CHECK(handle.valid());
      harness.draw(handle);
      harness.renderer.destroyMaterial(handle);
    }
    CHECK(reportErrors());
  }

  TEST("every node's shader should build and draw")
  {
    for (const graph::NodeType& type : graph::nodeTypes())
    {
      if (type.id == graph::kOutputType)
        continue;
      for (uint32_t output = 0; output < type.outputs.size(); ++output)
      {
        graph::Graph g;
        const graph::NodeId out = g.add(std::string(graph::kOutputType), {});
        const graph::NodeId node = g.add(type.id, {});
        g.connect(node, output, out, graph::kBaseColor);
        const MaterialHandle handle = harness.renderer.createMaterial(
            graphMaterial(g, AlphaMode::Opaque));
        CHECK(handle.valid());
        harness.draw(handle);
        harness.renderer.destroyMaterial(handle);
        if (!reportErrors())
        {
          std::fprintf(stderr,
                       "  ...from the %s node's output %u\n",
                       type.label.c_str(),
                       output);
          CHECK(false);
        }
      }
    }
  }

  TEST("graph materials should build and draw cut-out and see-through, with "
       "textures")
  {
    graph::Graph g = graph::Graph::makeDefault();
    const graph::NodeId texture = g.add("texture", {});
    const graph::NodeId noise = g.add("noise", {});
    g.connect(texture, 5, g.output(), graph::kAlpha);
    g.connect(noise, 0, g.output(), graph::kEmission);
    for (const AlphaMode mode : {AlphaMode::Opaque,
                                 AlphaMode::Mask,
                                 AlphaMode::Blend,
                                 AlphaMode::Additive})
    {
      MeshMaterial material = graphMaterial(g, mode);
      auto image = std::make_shared<TextureData>();
      image->width = image->height = 2;
      image->rgba.assign(16, 200);
      material.shaderTextures = {image};
      const MaterialHandle handle = harness.renderer.createMaterial(material);
      CHECK(handle.valid());
      harness.draw(handle);
      // Changing values keeps the shader, and pipelines.
      material.shaderParameters[0] = glm::vec4(0.25f);
      harness.renderer.updateMaterial(handle, material);
      harness.draw(handle);
      harness.renderer.destroyMaterial(handle);
    }
    CHECK(reportErrors());
  }

  TEST("every node should draw on the GPU what it works out on the CPU")
  {
    // Each output as pure emission: base colour black, matte, not metal, so
    // the lit pass draws exactly the emission (clamped to 0..1).
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    // A smooth 4x4 gradient for the nodes that read an image.
    auto image = std::make_shared<TextureData>();
    image->width = image->height = 4;
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        image->rgba.insert(
            image->rgba.end(),
            {uint8_t(x * 80), uint8_t(y * 80), uint8_t(200 - x * 40), 255});
    constexpr float kTime = 1.25f;

    for (const graph::NodeType& type : graph::nodeTypes())
    {
      if (type.id == graph::kOutputType)
        continue;
      for (uint32_t output = 0; output < type.outputs.size(); ++output)
      {
        graph::Graph g;
        const graph::NodeId out = g.add(std::string(graph::kOutputType), {});
        g.find(out)->inputs[graph::kBaseColor] = glm::vec4(0.0f);
        const graph::NodeId node = g.add(type.id, {});
        g.find(node)->texture = "asset:image";
        g.connect(node, output, out, graph::kEmission);
        const graph::CompiledGraph compiled = graph::compile(g);
        MeshMaterial material = graphMaterial(g, AlphaMode::Opaque);
        std::vector<std::shared_ptr<const TextureData>> textures(
            compiled.textures.size(), image);
        material.shaderTextures = textures;

        const MaterialHandle handle = harness.renderer.createMaterial(material);
        const std::vector<uint8_t> pixels =
            drawPlane(harness, plane, handle, kTime);
        harness.renderer.destroyMaterial(handle);
        CHECK(pixels.size() == std::size_t(kSize.x) * kSize.y * 4);
        if (pixels.empty())
          continue;

        // The same points on the CPU. Cell edges (Voronoi, Checkerboard,
        // Floor) can land either side by a rounding: a few pixels may differ.
        int compared = 0, differing = 0;
        float worst = 0.0f;
        for (int y = 0; y < kSize.y; ++y)
          for (int x = 0; x < kSize.x; ++x)
          {
            graph::SurfacePoint point;
            point.uv = {(x + 0.5f) / kSize.x, (y + 0.5f) / kSize.y};
            point.position = {
                point.uv.x * 2.0f - 1.0f, 1.0f - point.uv.y * 2.0f, 0.0f};
            point.normal = {0.0f, 0.0f, 1.0f};
            point.tangent = {1.0f, 0.0f, 0.0f}; // +u along +x, up the image +y
            point.bitangent = {0.0f, 1.0f, 0.0f};
            point.toView = glm::normalize(kParityCamera - point.position);
            point.time = kTime;
            const glm::vec3 expected =
                compiled.shader->evaluate(point, compiled.parameters, textures)
                    .emission;
            if (!std::isfinite(expected.x) || !std::isfinite(expected.y) ||
                !std::isfinite(expected.z))
              continue; // NaN (a square root of a negative...): either way
            const uint8_t* pixel = &pixels[(std::size_t(y) * kSize.x + x) * 4];
            float difference = 0.0f;
            for (int c = 0; c < 3; ++c)
              difference =
                  std::max(difference,
                           std::abs(pixel[c] / 255.0f -
                                    std::clamp(expected[c], 0.0f, 1.0f)));
            ++compared;
            worst = std::max(worst, difference);
            if (difference > 0.03f)
              ++differing;
          }
        if (differing * 50 > compared) // more than 2%
        {
          std::fprintf(stderr,
                       "  %s output %u: %d of %d pixels differ (worst %.3f)\n",
                       type.label.c_str(),
                       output,
                       differing,
                       compared,
                       worst);
          CHECK(false);
        }
      }
    }
    CHECK(reportErrors());
  }

  TEST("without UVs, the GPU's tangent frame should be the CPU's (a triplanar "
       "normal map's)")
  {
    // The plane with its texture coordinates gone: the frame comes from the
    // world's axes.
    MeshData bare = meshes::plane({2.0f, 2.0f});
    for (MeshVertex& vertex : bare.vertices)
      vertex.uv = glm::vec2(0.0f);
    const MeshHandle plane = harness.renderer.createMesh(bare);
    auto image = std::make_shared<TextureData>();
    image->width = image->height = 2;
    image->rgba = {230,
                   100,
                   128,
                   255,
                   60,
                   200,
                   220,
                   255,
                   128,
                   128,
                   255,
                   255,
                   180,
                   60,
                   200,
                   255};
    image->linear = true;

    graph::Graph g;
    const graph::NodeId out = g.add(std::string(graph::kOutputType), {});
    g.find(out)->inputs[graph::kBaseColor] = glm::vec4(0.0f);
    const graph::NodeId bumps = g.add("triplanarNormalMap", {});
    g.connect(
        bumps, 0, out, graph::kEmission); // the tangent-space normal, as colour
    const graph::CompiledGraph compiled = graph::compile(g);
    MeshMaterial material = graphMaterial(g, AlphaMode::Opaque);
    const std::vector<std::shared_ptr<const TextureData>> textures = {image};
    material.shaderTextures = textures;
    const MaterialHandle handle = harness.renderer.createMaterial(material);
    const std::vector<uint8_t> pixels = drawPlane(harness, plane, handle, 0.0f);
    harness.renderer.destroyMaterial(handle);
    CHECK(!pixels.empty());

    int differing = 0;
    for (int y = 0; y < kSize.y && !pixels.empty(); ++y)
      for (int x = 0; x < kSize.x; ++x)
      {
        graph::SurfacePoint point;
        point.position = {(x + 0.5f) / kSize.x * 2.0f - 1.0f,
                          1.0f - (y + 0.5f) / kSize.y * 2.0f,
                          0.0f};
        point.normal = {0.0f, 0.0f, 1.0f};
        graph::tangentFrame(point.normal,
                            glm::vec3(0.0f),
                            glm::vec3(0.0f),
                            point.tangent,
                            point.bitangent);
        point.toView = glm::normalize(kParityCamera - point.position);
        const glm::vec3 expected =
            compiled.shader->evaluate(point, compiled.parameters, textures)
                .emission;
        const uint8_t* pixel = &pixels[(std::size_t(y) * kSize.x + x) * 4];
        for (int c = 0; c < 3; ++c)
          if (std::abs(pixel[c] / 255.0f -
                       std::clamp(expected[c], 0.0f, 1.0f)) > 0.03f)
          {
            ++differing;
            break;
          }
      }
    CHECK(differing * 50 <= kSize.x * kSize.y);
    CHECK(reportErrors());
  }

  TEST("bloom should spread light above full white, and leave the rest of a "
       "picture alone")
  {
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    // A small patch glowing at 4 x white, on a black sky.
    graph::Graph g;
    const graph::NodeId out = g.add(std::string(graph::kOutputType), {});
    g.find(out)->inputs[graph::kBaseColor] = glm::vec4(0.0f);
    const graph::NodeId bright = g.add("number", {});
    g.find(bright)->value.x = 4.0f;
    g.connect(bright, 0, out, graph::kEmission);
    const MaterialHandle glowing =
        harness.renderer.createMaterial(graphMaterial(g, AlphaMode::Opaque));
    MeshInstance patch;
    patch.model = glm::scale(glm::mat4(1.0f), glm::vec3(0.15f));
    const auto drawGlow = [&](bool bloom)
    {
      return drawPlane(
          harness,
          plane,
          glowing,
          0.0f,
          [bloom](FrameParams& frame)
          {
            frame.lighting.skyColor = glm::vec3(0.0f);
            frame.bloom.enabled = bloom;
          },
          patch);
    };
    const std::vector<uint8_t> withBloom = drawGlow(true),
                               without = drawGlow(false);
    CHECK(!withBloom.empty() && !without.empty());
    if (!withBloom.empty() && !without.empty())
    {
      // Just outside the patch (it spans the middle 15%): black without
      // bloom, glowing with it; the patch itself full white either way.
      const auto red = [](const std::vector<uint8_t>& pixels, int x, int y)
      { return int(pixels[(std::size_t(y) * kSize.x + x) * 4]); };
      const int middle = kSize.x / 2, outside = middle + kSize.x * 15 / 100;
      CHECK(red(without, outside, middle) == 0);
      CHECK(red(withBloom, outside, middle) > 10);
      CHECK(red(withBloom, middle, middle) == 255);
      CHECK(red(without, middle, middle) == 255);

      // The material's Bloom: 0, as bright but no glow around it; 3, more.
      const auto withAmount = [&](float amount)
      {
        graph::Graph tuned = g;
        tuned.find(out)->inputs[graph::kBloom] = glm::vec4(amount);
        const MaterialHandle material = harness.renderer.createMaterial(
            graphMaterial(tuned, AlphaMode::Opaque));
        const std::vector<uint8_t> pixels = drawPlane(
            harness,
            plane,
            material,
            0.0f,
            [](FrameParams& frame)
            { frame.lighting.skyColor = glm::vec3(0.0f); },
            patch);
        harness.renderer.destroyMaterial(material);
        return pixels;
      };
      const std::vector<uint8_t> none = withAmount(0.0f),
                                 more = withAmount(3.0f);
      CHECK(!none.empty() && !more.empty());
      if (!none.empty() && !more.empty())
      {
        CHECK(red(none, outside, middle) == 0);
        CHECK(red(none, middle, middle) == 255);
        CHECK(red(more, outside, middle) >
              red(withBloom, outside, middle) + 10);
      }
    }
    harness.renderer.destroyMaterial(glowing);

    // An ordinary lit surface (nothing above 1): the same with bloom or not.
    MeshMaterial painted;
    painted.baseColor = {0.9f, 0.6f, 0.3f, 1.0f};
    painted.roughness = 0.4f;
    const MaterialHandle plain = harness.renderer.createMaterial(painted);
    const auto drawPlain = [&](bool bloom)
    {
      return drawPlane(harness,
                       plane,
                       plain,
                       0.0f,
                       [bloom](FrameParams& frame)
                       { frame.bloom.enabled = bloom; });
    };
    CHECK(drawPlain(true) == drawPlain(false));
    harness.renderer.destroyMaterial(plain);
    CHECK(reportErrors());
  }

  // --- tone mapping and grading (the composite) -----------------------------

  // A plane glowing `light` x white all over (nothing lit: black base colour),
  // drawn without bloom through `grading`; the middle pixel's channels, and
  // a corner's red.
  struct Graded
  {
    int r = -1, g = -1, b = -1, corner = -1;
  };
  const MeshHandle gradePlane =
      harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
  const auto drawGraded =
      [&](float light,
          const ColorGrading& grading,
          const LightBalance& balance = {},
          const std::function<void(graph::Graph&, graph::NodeId)>& surface = {})
  {
    graph::Graph g;
    const graph::NodeId out = g.add(std::string(graph::kOutputType), {});
    g.find(out)->inputs[graph::kBaseColor] = glm::vec4(0.0f);
    g.find(out)->inputs[graph::kEmission] = glm::vec4(glm::vec3(light), 1.0f);
    if (surface)
      surface(g, out);
    const MaterialHandle material =
        harness.renderer.createMaterial(graphMaterial(g, AlphaMode::Opaque));
    const std::vector<uint8_t> pixels = drawPlane(harness,
                                                  gradePlane,
                                                  material,
                                                  0.0f,
                                                  [&](FrameParams& frame)
                                                  {
                                                    frame.bloom.enabled = false;
                                                    frame.grading = grading;
                                                    frame.lightBalance =
                                                        balance;
                                                  });
    harness.renderer.destroyMaterial(material);
    Graded result;
    if (pixels.empty())
      return result;
    const std::size_t middle =
        (std::size_t(kSize.y / 2) * kSize.x + kSize.x / 2) * 4;
    result.r = pixels[middle];
    result.g = pixels[middle + 1];
    result.b = pixels[middle + 2];
    result.corner = pixels[0];
    return result;
  };
  const auto grey = [](const Graded& p)
  { return p.r >= 0 && std::abs(p.r - p.g) <= 1 && std::abs(p.r - p.b) <= 1; };

  TEST("with no tone mapping the picture should be as it always was: clamped "
       "at white")
  {
    ColorGrading none;
    const Graded dim = drawGraded(0.18f, none);
    const Graded white = drawGraded(1.0f, none);
    const Graded bright = drawGraded(8.0f, none);
    CHECK(grey(dim) && grey(white) && grey(bright));
    CHECK(white.r == 255 && bright.r == 255);
    // 0.18 x 255 (the test's target is plain, not sRGB).
    CHECK(dim.r >= 40 && dim.r <= 52);
    // Graded but still not tone mapped: exposure brightens, still clamped.
    ColorGrading brighter;
    brighter.exposure = 1.0f;
    CHECK(drawGraded(0.18f, brighter).r > dim.r + 20);
    CHECK(drawGraded(8.0f, brighter).r == 255);
    CHECK(reportErrors());
  }

  // Each tone mapper: grey stays grey, brighter light stays brighter (never
  // clipping as None does at 1), and mid-grey lands near where it was.
  const auto checkToneMapper =
      [&](ToneMapper mapper, int greyLow, int greyHigh, int whiteBelow)
  {
    ColorGrading grading;
    grading.toneMapper = mapper;
    const Graded dim = drawGraded(0.18f, grading);
    const Graded white = drawGraded(1.0f, grading);
    const Graded bright = drawGraded(8.0f, grading);
    const Graded blazing = drawGraded(64.0f, grading);
    CHECK(grey(dim) && grey(white) && grey(bright));
    CHECK(dim.r >= greyLow && dim.r <= greyHigh);
    CHECK(dim.r < white.r && white.r < bright.r && bright.r <= blazing.r);
    CHECK(white.r < whiteBelow); // rolled off, not clipped at white
    CHECK(bright.r > 200);
    if (dim.r < greyLow || dim.r > greyHigh || white.r >= whiteBelow)
      std::printf("    (tone mapper %d: 0.18 -> %d, 1 -> %d, 8 -> %d)\n",
                  static_cast<int>(mapper),
                  dim.r,
                  white.r,
                  bright.r);
    CHECK(reportErrors());
  };

  TEST("ACES should roll highlights off with filmic contrast")
  {
    checkToneMapper(ToneMapper::Aces, 48, 62, 230);
  }

  TEST("AgX should roll highlights off, keeping mid-grey")
  {
    checkToneMapper(ToneMapper::AgX, 48, 64, 200);
  }

  TEST("PBR Neutral should keep colours and only compress the brightest")
  {
    checkToneMapper(ToneMapper::Neutral, 30, 42, 240);
    // A saturated colour below the knee stays (nearly) as it was: its hue
    // kept, only the small offset taken off.
    ColorGrading neutral;
    neutral.toneMapper = ToneMapper::Neutral;
    const auto red = [](graph::Graph& g, graph::NodeId out)
    { g.find(out)->inputs[graph::kEmission] = glm::vec4(0.5f, 0.1f, 0.1f, 1); };
    const Graded mapped = drawGraded(0.0f, neutral, {}, red);
    const Graded plain = drawGraded(0.0f, ColorGrading{}, {}, red);
    CHECK(mapped.r > 0 && std::abs(mapped.r - plain.r) < 20);
    CHECK(mapped.r > mapped.g + 60);
    CHECK(reportErrors());
  }

  TEST("white balance, saturation and the vignette should grade the picture")
  {
    ColorGrading warm;
    warm.temperature = 0.6f;
    const Graded warmed = drawGraded(0.3f, warm);
    CHECK(warmed.r > warmed.b + 15);
    ColorGrading cool;
    cool.temperature = -0.6f;
    const Graded cooled = drawGraded(0.3f, cool);
    CHECK(cooled.b > cooled.r + 15);

    ColorGrading desaturated;
    desaturated.saturation = 0.0f;
    const auto red = [](graph::Graph& g, graph::NodeId out)
    { g.find(out)->inputs[graph::kEmission] = glm::vec4(0.6f, 0.1f, 0.1f, 1); };
    const Graded greyed = drawGraded(0.0f, desaturated, {}, red);
    CHECK(std::abs(greyed.r - greyed.g) <= 1 &&
          std::abs(greyed.r - greyed.b) <= 1);

    ColorGrading vignette;
    vignette.vignette = 1.0f;
    const Graded framed = drawGraded(0.5f, vignette);
    CHECK(framed.corner < framed.r - 60);
    CHECK(reportErrors());
  }

  TEST("a visual style's light balance should scale the sun's and ambient's "
       "shares of a lit surface")
  {
    // A white matte plane in sunlight, from straight above; no emission.
    const auto lit = [&](const LightBalance& balance)
    {
      return drawGraded(0.0f,
                        ColorGrading{},
                        balance,
                        [](graph::Graph& g, graph::NodeId out)
                        {
                          g.find(out)->inputs[graph::kBaseColor] =
                              glm::vec4(0.6f, 0.6f, 0.6f, 1.0f);
                          g.find(out)->inputs[graph::kEmission] =
                              glm::vec4(0.0f);
                        });
    };
    const Graded plain = lit({});
    const Graded noSun = lit({.sun = glm::vec3(0.0f)});
    const Graded blueAmbient = lit({.ambient = glm::vec3(0.2f, 0.2f, 1.0f)});
    CHECK(plain.r > 0);
    CHECK(noSun.r < plain.r - 10);
    CHECK(blueAmbient.b > blueAmbient.r + 10);
    // Neutral balance is exactly the unbalanced picture.
    CHECK(lit(LightBalance{}).r == plain.r);
    CHECK(reportErrors());
  }
  harness.renderer.destroyMesh(gradePlane);

  TEST("point lights' shadows should fall away from them, whichever way they "
       "point")
  {
    // A floor (or, looking up, a ceiling) lit at night by one light, with a
    // small box between the light and one spot of it. That spot should be in
    // shadow and the spot opposite it, as far from the light, not: so each
    // cube face the light's shadow map has must look the right way round.
    const MeshHandle floorMesh =
        harness.renderer.createMesh(meshes::plane({6.0f, 6.0f}));
    const MeshData boxData = meshes::box({0.4f, 0.4f, 0.4f});
    const glm::vec3 boxCenter = boxData.bounds().center();
    const MeshHandle box = harness.renderer.createMesh(boxData);
    MeshMaterial grey;
    grey.baseColor = {0.7f, 0.7f, 0.7f, 1.0f};
    const MaterialHandle material = harness.renderer.createMaterial(grey);

    struct Case
    {
      const char* name;
      glm::vec3 light;
      glm::vec3 spot; // where the box's shadow falls; its opposite is lit
      bool ceiling;   // the plane faces down, seen from below
    };
    const Case cases[] = {
        {"down, along x (-Z face)",
         {0.0f, 0.0f, 2.0f},
         {1.6f, 0.0f, 0.0f},
         false},
        {"down, along y (-Z face)",
         {0.0f, 0.0f, 2.0f},
         {0.0f, 1.6f, 0.0f},
         false},
        {"+X face", {0.0f, 0.0f, 0.5f}, {2.0f, 0.0f, 0.0f}, false},
        {"-X face", {0.0f, 0.0f, 0.5f}, {-2.0f, 0.0f, 0.0f}, false},
        {"+Y face", {0.0f, 0.0f, 0.5f}, {0.0f, 2.0f, 0.0f}, false},
        {"-Y face", {0.0f, 0.0f, 0.5f}, {0.0f, -2.0f, 0.0f}, false},
        {"up, along x (+Z face)",
         {0.0f, 0.0f, -2.0f},
         {1.6f, 0.0f, 0.0f},
         true},
        {"up, along y (+Z face)",
         {0.0f, 0.0f, -2.0f},
         {0.0f, 1.6f, 0.0f},
         true},
    };
    for (const Case& test : cases)
      for (const bool shadows : {true, false})
      {
        // A backend without them (wgpu's OpenGL fallback) lights unshadowed.
        if (shadows && !harness.renderer.capabilities().pointLightShadows)
          continue;
        PointLight light;
        light.position = test.light;
        light.radius = 6.0f;
        light.intensity = 2.0f;
        light.color = glm::vec3(1.0f);
        light.castsShadow = shadows;
        // The box halfway along the light's ray to the spot.
        MeshInstance caster;
        caster.model = glm::translate(
            glm::mat4(1.0f), (test.light + test.spot) * 0.5f - boxCenter);
        MeshBatch casters;
        casters.mesh = box;
        casters.lightShadowInstances = {&caster, 1};

        MeshInstance surface;
        if (test.ceiling)
          surface.model = glm::rotate(
              glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
        glm::mat4 viewProjection(1.0f);
        const std::vector<uint8_t> pixels =
            drawPlane(harness,
                      floorMesh,
                      material,
                      0.0f,
                      [&](FrameParams& frame)
                      {
                        const float side = test.ceiling ? -1.0f : 1.0f;
                        frame.cameraPosition = {0.0f, 0.0f, 5.0f * side};
                        frame.cameraForward = {0.0f, 0.0f, -side};
                        frame.view = glm::lookAt(frame.cameraPosition,
                                                 glm::vec3(0.0f),
                                                 glm::vec3(0.0f, 1.0f, 0.0f));
                        frame.projection = glm::orthoRH_ZO(
                            -3.0f, 3.0f, -3.0f, 3.0f, 0.1f, 10.0f);
                        frame.viewProjection = frame.projection * frame.view;
                        viewProjection = frame.viewProjection;
                        frame.lighting.ambient = 0.05f;
                        frame.lighting.sun.diffuseStrength = 0.0f;
                        frame.lighting.sun.direction = {0.0f, 0.0f, -1.0f};
                        frame.pointLights = {&light, 1};
                        frame.bloom.enabled = false;
                      },
                      surface,
                      {&casters, 1});
        CHECK(!pixels.empty());
        if (pixels.empty())
          continue;
        const auto brightness = [&](const glm::vec3& world)
        {
          const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
          const int x = std::clamp(
              int((clip.x / clip.w * 0.5f + 0.5f) * kSize.x), 0, kSize.x - 1);
          const int y = std::clamp(
              int((0.5f - clip.y / clip.w * 0.5f) * kSize.y), 0, kSize.y - 1);
          const uint8_t* p = &pixels[(std::size_t(y) * kSize.x + x) * 4];
          return int(p[0]) + p[1] + p[2];
        };
        const glm::vec3 opposite(-test.spot.x, -test.spot.y, test.spot.z);
        const int shaded = brightness(test.spot), lit = brightness(opposite);
        const bool right =
            shadows ? lit - shaded > 60 : std::abs(lit - shaded) < 12;
        CHECK(right);
        if (!right)
          std::fprintf(stderr,
                       "  %s, shadows %s: spot %d, opposite %d\n",
                       test.name,
                       shadows ? "on" : "off",
                       shaded,
                       lit);
      }

    // Something nearer the light than its shadowNear (a glowing object's own
    // surface, for its light) doesn't block it.
    {
      PointLight light;
      light.position = {0.0f, 0.0f, 2.0f};
      light.radius = 6.0f;
      light.intensity = 2.0f;
      light.castsShadow = true;
      light.shadowNear = 0.6f;
      MeshInstance caster;
      caster.model = glm::translate(
          glm::mat4(1.0f),
          light.position + glm::vec3(0.2f, 0.0f, -0.4f) - boxCenter);
      MeshBatch casters;
      casters.mesh = box;
      casters.lightShadowInstances = {&caster, 1};
      const std::vector<uint8_t> pixels =
          drawPlane(harness,
                    floorMesh,
                    material,
                    0.0f,
                    [&](FrameParams& frame)
                    {
                      frame.projection = glm::orthoRH_ZO(
                          -3.0f, 3.0f, -3.0f, 3.0f, 0.1f, 10.0f);
                      frame.viewProjection = frame.projection * frame.view;
                      frame.lighting.ambient = 0.05f;
                      frame.lighting.sun.diffuseStrength = 0.0f;
                      frame.pointLights = {&light, 1};
                      frame.bloom.enabled = false;
                    },
                    {},
                    {&casters, 1});
      // Straight below the light, past the box: lit as brightly as beside it.
      CHECK(!pixels.empty());
      if (!pixels.empty())
      {
        const auto red = [&](int x, int y)
        { return int(pixels[(std::size_t(y) * kSize.x + x) * 4]); };
        const int below = red(kSize.x / 2 + 3, kSize.y / 2),
                  beside = red(kSize.x / 2 - 3, kSize.y / 2);
        CHECK(std::abs(below - beside) < 6);
      }
    }
    harness.renderer.destroyMaterial(material);
    CHECK(reportErrors());
  }

  TEST("metal should reflect point lights, where it has no diffuse colour to "
       "light")
  {
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    MeshMaterial metal;
    metal.baseColor = {0.9f, 0.8f, 0.5f, 1.0f};
    metal.metallic = 1.0f;
    metal.roughness = 0.5f;
    const MaterialHandle material = harness.renderer.createMaterial(metal);
    PointLight light;
    light.position = {0.0f, 0.0f, 1.0f};
    light.radius = 4.0f;
    light.intensity = 2.0f;
    light.color = glm::vec3(1.0f);
    const auto draw = [&](bool lit)
    {
      return drawPlane(harness,
                       plane,
                       material,
                       0.0f,
                       [&](FrameParams& frame)
                       {
                         frame.lighting.ambient = 0.05f;
                         frame.lighting.sun.diffuseStrength = 0.0f;
                         if (lit)
                           frame.pointLights = {&light, 1};
                         frame.bloom.enabled = false;
                       });
    };
    const std::vector<uint8_t> dark = draw(false), shining = draw(true);
    CHECK(!dark.empty() && !shining.empty());
    if (!dark.empty() && !shining.empty())
    {
      // Under the light, where its highlight is: much brighter.
      const std::size_t middle =
          (std::size_t(kSize.y / 2) * kSize.x + kSize.x / 2) * 4;
      CHECK(int(shining[middle]) > int(dark[middle]) + 80);
      // And in a dimmer light (the bright one's highlight is past white), the
      // metal's own colour: gold.
      light.intensity = 0.15f;
      const std::vector<uint8_t> dim = draw(true);
      CHECK(!dim.empty() && dim[middle] > dim[middle + 2] + 30);
    }

    // Mirror-smooth, a big light (a glowing panel's) shows a big highlight,
    // where a point shows a speck.
    metal.roughness = 0.05f;
    harness.renderer.updateMaterial(material, metal);
    light.intensity = 1.0f;
    const auto highlightPixels = [&](float sourceRadius)
    {
      light.sourceRadius = sourceRadius;
      const std::vector<uint8_t> pixels = draw(true);
      int bright = 0;
      for (std::size_t i = 0; i < pixels.size(); i += 4)
        bright += pixels[i] > 128;
      return bright;
    };
    const int speck = highlightPixels(0.0f), panel = highlightPixels(0.4f);
    CHECK(panel > speck * 4 && panel > 40);
    harness.renderer.destroyMaterial(material);
    CHECK(reportErrors());
  }

  TEST("grass should stand up, sway in the wind, lie flat where trampled, and "
       "stop at its distance")
  {
    // A 2 x 2 lawn seen from the side, low down, against a black sky.
    const MeshData lawn = meshes::plane({2.0f, 2.0f});
    std::vector<glm::vec3> positions;
    for (const MeshVertex& vertex : lawn.vertices)
      positions.push_back(vertex.position);
    const GrassGround ground =
        GrassGround::build(positions, lawn.indices, glm::mat4(1.0f));
    const GrassHandle grass = harness.renderer.createGrass(ground);
    CHECK(grass.valid());
    const MeshHandle plane = harness.renderer.createMesh(lawn);
    MeshMaterial green;
    green.baseColor = {0.3f, 0.7f, 0.2f, 1.0f};
    const MaterialHandle material = harness.renderer.createMaterial(green);

    glm::mat4 viewProjection(1.0f);
    // `box`: the view's left, right, bottom and top.
    const auto draw = [&](float wind,
                          float drawDistance,
                          MaterialHandle look,
                          glm::vec4 box = glm::vec4(-1.2f, 1.2f, -0.15f, 0.55f))
    {
      const GrassBatch batch{grass,
                             look,
                             plane,
                             glm::mat4(1.0f),
                             {.density = 300.0f, .height = 0.3f},
                             0.0f,
                             drawDistance};
      return drawPlane(
          harness,
          plane,
          material,
          2.0f,
          [&](FrameParams& frame)
          {
            frame.cameraPosition = {0.0f, -5.0f, 0.2f};
            frame.cameraForward = {0.0f, 1.0f, 0.0f};
            frame.view = glm::lookAt(frame.cameraPosition,
                                     glm::vec3(0.0f, 0.0f, 0.2f),
                                     glm::vec3(0.0f, 0.0f, 1.0f));
            frame.projection =
                glm::orthoRH_ZO(box.x, box.y, box.z, box.w, 0.1f, 10.0f);
            frame.viewProjection = frame.projection * frame.view;
            viewProjection = frame.viewProjection;
            frame.lighting.skyColor = glm::vec3(0.0f);
            frame.wind = {
                .direction = 0.0f, .strength = wind, .gustiness = 0.0f};
            frame.bloom.enabled = false;
          },
          {},
          {},
          {&batch, 1});
    };
    // Grass pixels standing above a height (by default, a blade's flattened
    // height), in the last view drawn.
    const auto standing =
        [&](const std::vector<uint8_t>& pixels, float above = 0.1f)
    {
      const glm::vec4 clip =
          viewProjection * glm::vec4(0.0f, 0.0f, above, 1.0f);
      const int rowAt = int((0.5f - clip.y / clip.w * 0.5f) * kSize.y);
      int count = 0;
      for (int y = 0; y < rowAt; ++y)
        for (int x = 0; x < kSize.x; ++x)
          count += pixels[(std::size_t(y) * kSize.x + x) * 4 + 1] > 20;
      return count;
    };

    const std::vector<uint8_t> still = draw(0.0f, 40.0f, material);
    CHECK(!still.empty());
    if (still.empty())
      return testing::report("webGpuRendererTests");
    const int upright = standing(still);
    CHECK(upright > 200);

    // In a gale: blades lean over, a different picture.
    const std::vector<uint8_t> windy = draw(1.0f, 40.0f, material);
    int differing = 0;
    for (std::size_t i = 0; i < still.size(); i += 4)
      differing += std::abs(int(still[i + 1]) - int(windy[i + 1])) > 10;
    CHECK(differing > 100);

    // Trampled flat all over: hardly anything stands.
    const glm::ivec2 size(8, 8);
    std::vector<uint8_t> flat(std::size_t(size.x) * size.y * 4);
    for (std::size_t i = 0; i < flat.size(); i += 4)
    {
      flat[i] = 255; // pressed toward +x
      flat[i + 1] = 128;
      flat[i + 2] = 255; // all the way down
      flat[i + 3] = 255;
    }
    harness.renderer.updateGrassTrample(
        grass, GrassTrampleImage{{-1.5f, -1.5f}, {1.5f, 1.5f}, size, flat});
    const int trampled = standing(draw(0.0f, 40.0f, material));
    CHECK(trampled < upright / 5);

    // Beyond its draw distance (5 away, drawn to 2): none at all.
    CHECK(standing(draw(0.0f, 2.0f, material)) == 0);
    harness.renderer.updateGrassTrample(
        grass,
        GrassTrampleImage{{-1.5f, -1.5f},
                          {1.5f, 1.5f},
                          {1, 1},
                          std::vector<uint8_t>{128, 128, 0, 255}});
    CHECK(standing(draw(0.0f, 40.0f, material)) > upright * 4 / 5);

    // Its maps: mown short on the left half, bare on the right.
    GrassMaps maps(ground.grid());
    maps.stamp(GrassMaps::Length,
               {-1.0f, 0.0f},
               1.0f,
               0.2f,
               GrassMaps::Blend::Toward,
               1.0f,
               0.0f);
    std::vector<uint8_t> shape, data;
    maps.pixels({0, 0}, maps.grid().size, shape, data);
    harness.renderer.updateGrassMaps(
        grass,
        GrassMapsImage{maps.grid(), {0, 0}, maps.grid().size, shape, data});
    const int mown = standing(draw(0.0f, 40.0f, material));
    CHECK(mown < upright * 3 / 4 && mown > upright / 4);
    maps.fill(GrassMaps::Coverage, 0.0f);
    maps.pixels({0, 0}, maps.grid().size, shape, data);
    harness.renderer.updateGrassMaps(
        grass,
        GrassMapsImage{maps.grid(), {0, 0}, maps.grid().size, shape, data});
    CHECK(standing(draw(0.0f, 40.0f, material)) == 0);
    // Mown short all over: it hardly moves in the gale that bent it long.
    maps.fill(GrassMaps::Coverage, 1.0f);
    maps.fill(GrassMaps::Length, 0.35f);
    maps.pixels({0, 0}, maps.grid().size, shape, data);
    harness.renderer.updateGrassMaps(
        grass,
        GrassMapsImage{maps.grid(), {0, 0}, maps.grid().size, shape, data});
    // Seen close up (heights -0.02 to 0.18: the camera's 0.2 up), the wind
    // bends long grass well below its height, but short grass hardly at all.
    const glm::vec4 close(-0.3f, 0.3f, -0.22f, -0.02f);
    const int shortStill = standing(draw(0.0f, 40.0f, material, close), 0.08f);
    const int shortWindy = standing(draw(1.0f, 40.0f, material, close), 0.08f);
    CHECK(shortStill > 20);
    CHECK(shortWindy > shortStill * 4 / 5);

    // Unpainted again: all back.
    harness.renderer.updateGrassMaps(grass, GrassMapsImage{});
    CHECK(standing(draw(0.0f, 40.0f, material)) > upright * 4 / 5);

    // A node-graph material colours grass too (its pipeline builds).
    graph::Graph g;
    const graph::NodeId out = g.add(std::string(graph::kOutputType), {});
    const graph::NodeId uv = g.add("uv", {});
    const graph::NodeId split = g.add("split", {});
    g.connect(uv, 0, split, 0);
    g.connect(split, 1, out, graph::kBaseColor); // darker up the blade
    const MaterialHandle graphLook =
        harness.renderer.createMaterial(graphMaterial(g, AlphaMode::Opaque));
    CHECK(!draw(0.0f, 40.0f, graphLook).empty());

    harness.renderer.destroyGrass(grass);
    harness.renderer.destroyMaterial(graphLook);
    harness.renderer.destroyMaterial(material);
    CHECK(reportErrors());
  }

  TEST("grass should tint the ground it covers in its own colour")
  {
    // A white 2 x 2 plane seen from above, grass too short to see on it:
    // green where it covers it (the left half), white where it's bare.
    const MeshData lawn = meshes::plane({2.0f, 2.0f});
    std::vector<glm::vec3> positions;
    for (const MeshVertex& vertex : lawn.vertices)
      positions.push_back(vertex.position);
    const GrassGround ground =
        GrassGround::build(positions, lawn.indices, glm::mat4(1.0f));
    const GrassHandle grass = harness.renderer.createGrass(ground);
    const MeshHandle plane = harness.renderer.createMesh(lawn);
    MeshMaterial white;
    white.baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    MeshMaterial green;
    green.baseColor = {0.1f, 0.8f, 0.1f, 1.0f};
    const MaterialHandle whiteLook = harness.renderer.createMaterial(white);
    const MaterialHandle greenLook = harness.renderer.createMaterial(green);
    GrassMaps maps(ground.grid());
    for (int y = 0; y < maps.grid().size.y; ++y)
      for (int x = maps.grid().size.x / 2; x < maps.grid().size.x; ++x)
        maps.set(GrassMaps::Coverage, x, y, 0.0f);
    std::vector<uint8_t> shape, data;
    maps.pixels({0, 0}, maps.grid().size, shape, data);
    harness.renderer.updateGrassMaps(
        grass,
        GrassMapsImage{maps.grid(), {0, 0}, maps.grid().size, shape, data});

    const GrassBatch batch{grass,
                           greenLook,
                           plane,
                           glm::mat4(1.0f),
                           {.density = 50.0f, .height = 0.001f},
                           0.4f,
                           40.0f};
    const std::vector<uint8_t> pixels =
        drawPlane(harness,
                  plane,
                  whiteLook,
                  0.0f,
                  [](FrameParams& frame) { frame.bloom.enabled = false; },
                  {},
                  {},
                  {&batch, 1});
    CHECK(!pixels.empty());
    if (!pixels.empty())
    {
      const auto at = [&](int x, int y)
      { return &pixels[(std::size_t(y) * kSize.x + x) * 4]; };
      const uint8_t* covered = at(kSize.x / 4, kSize.y / 2);
      const uint8_t* bare = at(kSize.x * 3 / 4, kSize.y / 2);
      CHECK(covered[1] > covered[0] + 60);               // green
      CHECK(std::abs(int(bare[0]) - int(bare[1])) < 12); // white (lit grey)
    }
    harness.renderer.destroyGrass(grass);
    harness.renderer.destroyMaterial(whiteLook);
    harness.renderer.destroyMaterial(greenLook);
    CHECK(reportErrors());
  }

  TEST("a grass material should see its maps' data, on the blades and the "
       "ground")
  {
    // A grey 2 x 2 plane from above, under grass whose material is as bright
    // as its Data 0: painted 1 on the left half, 0 on the right.
    const MeshData lawn = meshes::plane({2.0f, 2.0f});
    std::vector<glm::vec3> positions;
    for (const MeshVertex& vertex : lawn.vertices)
      positions.push_back(vertex.position);
    const GrassGround ground =
        GrassGround::build(positions, lawn.indices, glm::mat4(1.0f));
    const GrassHandle grass = harness.renderer.createGrass(ground);
    const MeshHandle plane = harness.renderer.createMesh(lawn);
    MeshMaterial grey;
    grey.baseColor = {0.5f, 0.5f, 0.5f, 1.0f};
    const MaterialHandle greyLook = harness.renderer.createMaterial(grey);
    graph::Graph g;
    const graph::NodeId out = g.add(std::string(graph::kOutputType), {});
    const graph::NodeId node = g.add("grass", {});
    g.connect(node, 4, out, graph::kBaseColor); // data 0
    const MaterialHandle dataLook =
        harness.renderer.createMaterial(graphMaterial(g, AlphaMode::Opaque));
    GrassMaps maps(ground.grid());
    for (int y = 0; y < maps.grid().size.y; ++y)
      for (int x = 0; x < maps.grid().size.x / 2; ++x)
        maps.set(GrassMaps::Data0, x, y, 1.0f);
    std::vector<uint8_t> shape, data;
    maps.pixels({0, 0}, maps.grid().size, shape, data);
    harness.renderer.updateGrassMaps(
        grass,
        GrassMapsImage{maps.grid(), {0, 0}, maps.grid().size, shape, data});

    const auto draw = [&](float height)
    {
      const GrassBatch batch{grass,
                             dataLook,
                             plane,
                             glm::mat4(1.0f),
                             {.density = 200.0f, .height = height},
                             0.4f,
                             40.0f};
      return drawPlane(harness,
                       plane,
                       greyLook,
                       0.0f,
                       [](FrameParams& frame) { frame.bloom.enabled = false; },
                       {},
                       {},
                       {&batch, 1});
    };
    // The ground alone (blades too short to see), then blades over it.
    for (const float height : {0.001f, 0.3f})
    {
      const std::vector<uint8_t> pixels = draw(height);
      CHECK(!pixels.empty());
      if (pixels.empty())
        break;
      const auto brightness = [&](int x0, int x1)
      {
        int sum = 0;
        for (int y = kSize.y / 4; y < kSize.y * 3 / 4; ++y)
          for (int x = x0; x < x1; ++x)
            sum += pixels[(std::size_t(y) * kSize.x + x) * 4 + 1];
        return sum / ((x1 - x0) * kSize.y / 2);
      };
      const int wet = brightness(kSize.x / 8, kSize.x * 3 / 8);
      const int dry = brightness(kSize.x * 5 / 8, kSize.x * 7 / 8);
      CHECK(wet > dry + 60);
    }
    harness.renderer.destroyGrass(grass);
    harness.renderer.destroyMaterial(greyLook);
    harness.renderer.destroyMaterial(dataLook);
    CHECK(reportErrors());
  }

  TEST("edges should be anti-aliased: blended where a pixel is partly "
       "covered")
  if (harness.renderer.capabilities().antiAliasing)
  {
    // A white square turned 30 degrees, small enough to sit inside the view,
    // against a black sky: its slanted edges cross pixels part way.
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    MeshMaterial white;
    white.baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    const MaterialHandle material = harness.renderer.createMaterial(white);
    MeshInstance turned;
    turned.model =
        glm::rotate(
            glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(0.6f));
    const std::vector<uint8_t> pixels = drawPlane(
        harness,
        plane,
        material,
        0.0f,
        [](FrameParams& frame)
        {
          frame.lighting.skyColor = glm::vec3(0.0f);
          frame.bloom.enabled = false;
        },
        turned);
    CHECK(!pixels.empty());
    int inside = 0, between = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
      inside += pixels[i] > 200;
      between += pixels[i] > 25 && pixels[i] < 175;
    }
    CHECK(inside > 300);
    // Without anti-aliasing every pixel is all square or all sky.
    CHECK(between > 40);
    harness.renderer.destroyMaterial(material);
    CHECK(reportErrors());
  }

  TEST("UI should be drawn over the frame: boxes with smooth edges, and text "
       "from its texture")
  {
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    MeshMaterial black;
    black.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
    const MaterialHandle material = harness.renderer.createMaterial(black);

    // A red box in the top left, rounded; a glyph (half covered) in white
    // from a texture, bottom right.
    UiTexture texture;
    texture.id = 7;
    texture.size = {2, 2};
    texture.pixels.assign(16, 255);
    for (int i = 0; i < 4; ++i)
      texture.pixels[i * 4 + 3] = 128; // half coverage
    UiQuad box;
    box.rect = {4.0f, 4.0f, 24.0f, 24.0f};
    box.color = {1.0f, 0.0f, 0.0f, 1.0f};
    box.radius = 8.0f;
    UiQuad glyph;
    glyph.kind = UiQuad::Glyph;
    glyph.texture = &texture;
    glyph.rect = {40.0f, 40.0f, 16.0f, 16.0f};
    glyph.uv = {0.0f, 0.0f, 1.0f, 1.0f};
    glyph.color = {1.0f, 1.0f, 1.0f, 1.0f};
    const UiQuad quads[] = {box, glyph};
    const std::vector<uint8_t> pixels = drawPlane(
        harness,
        plane,
        material,
        0.0f,
        [](FrameParams& frame)
        {
          frame.lighting.skyColor = glm::vec3(0.0f);
          frame.bloom.enabled = false;
        },
        {},
        {},
        {},
        quads);
    CHECK(!pixels.empty());
    if (!pixels.empty())
    {
      const auto at = [&](int x, int y)
      { return &pixels[(std::size_t(y) * kSize.x + x) * 4]; };
      CHECK(at(16, 16)[0] > 240 && at(16, 16)[1] < 15); // inside: red
      CHECK(at(30, 16)[0] < 15);                        // beside: not
      CHECK(at(4, 4)[0] < 60); // a rounded corner: (nearly) not
      // Partly covered along its rounded corner.
      bool soft = false;
      for (int i = 4; i < 12; ++i)
        soft = soft || (at(i, i)[0] > 30 && at(i, i)[0] < 220);
      CHECK(soft);
      // The glyph: white at half its coverage (as the screen shows it).
      CHECK(at(48, 48)[0] > 90 && at(48, 48)[0] < 230 &&
            std::abs(int(at(48, 48)[0]) - int(at(48, 48)[2])) < 10);
    }
    harness.renderer.destroyMaterial(material);
    CHECK(reportErrors());
  }

  TEST("haze should fade far things into the sky")
  {
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    MeshMaterial red;
    red.baseColor = {1.0f, 0.0f, 0.0f, 1.0f};
    const MaterialHandle material = harness.renderer.createMaterial(red);
    // The plane is 5 from the camera; the sky blue.
    const auto draw = [&](float strength)
    {
      return drawPlane(harness,
                       plane,
                       material,
                       0.0f,
                       [&](FrameParams& frame)
                       {
                         frame.bloom.enabled = false;
                         frame.lighting.skyColor = {0.0f, 0.0f, 1.0f};
                         frame.haze = {.strength = strength, .distance = 2.0f};
                       });
    };
    const std::vector<uint8_t> clear = draw(0.0f);
    const std::vector<uint8_t> hazy = draw(1.0f);
    CHECK(!clear.empty() && !hazy.empty());
    if (!clear.empty() && !hazy.empty())
    {
      const std::size_t middle =
          (std::size_t(kSize.y / 2) * kSize.x + kSize.x / 2) * 4;
      CHECK(clear[middle] > 100 && clear[middle + 2] < 30);
      CHECK(hazy[middle] < 20 && hazy[middle + 2] > 220);
    }
    harness.renderer.destroyMaterial(material);
    CHECK(reportErrors());
  }

  TEST("depth of field should blur what's out of focus, and leave what's in "
       "it sharp")
  if (harness.renderer.capabilities().depthOfField)
  {
    // A white plane with a black square on its right: a hard edge, 5 from
    // the camera.
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    MeshMaterial white;
    white.baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    MeshMaterial black;
    black.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
    const MaterialHandle whiteLook = harness.renderer.createMaterial(white);
    const MaterialHandle blackLook = harness.renderer.createMaterial(black);
    MeshInstance square;
    square.model = glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, 0.01f}) *
                   glm::scale(glm::mat4(1.0f), {0.5f, 0.5f, 1.0f});
    const MaterialHandle blackMaterials[] = {blackLook};
    MeshBatch squareBatch;
    squareBatch.mesh = plane;
    squareBatch.materials = blackMaterials;
    squareBatch.instances = {&square, 1};

    const auto edge = [&](float focusDistance, bool enabled)
    {
      const std::vector<uint8_t> pixels =
          drawPlane(harness,
                    plane,
                    whiteLook,
                    0.0f,
                    [&](FrameParams& frame)
                    {
                      frame.bloom.enabled = false;
                      frame.depthOfField = {
                          .enabled = enabled, .range = 0.5f, .blur = 3.0f};
                      frame.focusDistance = focusDistance;
                    },
                    {},
                    {&squareBatch, 1});
      // The steepest step along the middle row.
      int steepest = 0;
      const std::size_t row = std::size_t(kSize.y / 2) * kSize.x;
      for (int x = 0; x + 1 < kSize.x; ++x)
        steepest = std::max(steepest,
                            std::abs(int(pixels[(row + x) * 4]) -
                                     int(pixels[(row + x + 1) * 4])));
      return pixels.empty() ? -1 : steepest;
    };
    const int sharp = edge(5.0f, false);
    CHECK(sharp > 150);
    CHECK(edge(5.0f, true) > sharp * 9 / 10); // in focus: still sharp
    CHECK(edge(40.0f, true) < sharp * 2 / 3); // focused far beyond: blurred
    CHECK(edge(0.5f, true) < sharp * 2 / 3);  // and right up close
    harness.renderer.destroyMaterial(whiteLook);
    harness.renderer.destroyMaterial(blackLook);
    CHECK(reportErrors());
  }

  TEST("hundreds of point lights should each light their own spot")
  {
    // A 40 x 40 plane seen from above at night, a 20 x 20 grid of small lights
    // over it: far past the old 32.
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({40.0f, 40.0f}));
    MeshMaterial grey;
    grey.baseColor = {0.6f, 0.6f, 0.6f, 1.0f};
    const MaterialHandle material = harness.renderer.createMaterial(grey);
    std::vector<PointLight> lights;
    for (int y = 0; y < 20; ++y)
      for (int x = 0; x < 20; ++x)
      {
        PointLight light;
        light.position = {-19.0f + x * 2.0f, -19.0f + y * 2.0f, 0.4f};
        light.radius = 0.9f;
        light.intensity = 2.0f;
        light.color = {1.0f, 1.0f, 1.0f};
        lights.push_back(light);
      }
    const std::vector<uint8_t> pixels =
        drawPlane(harness,
                  plane,
                  material,
                  0.0f,
                  [&lights](FrameParams& frame)
                  {
                    frame.projection = glm::orthoRH_ZO(
                        -20.0f, 20.0f, -20.0f, 20.0f, 0.1f, 10.0f);
                    frame.viewProjection = frame.projection * frame.view;
                    frame.lighting.ambient = 0.05f;
                    frame.lighting.sun.diffuseStrength = 0.0f;
                    frame.lighting.sun.direction = {0.0f, 0.0f, -1.0f};
                    frame.pointLights = lights;
                    frame.bloom.enabled = false;
                  });
    CHECK(!pixels.empty());
    if (!pixels.empty())
    {
      // Each light's spot against the dark between it and the next.
      const auto brightness = [&](glm::vec2 world)
      {
        const int x = std::clamp(
            int((world.x + 20.0f) / 40.0f * kSize.x), 0, kSize.x - 1);
        const int y = std::clamp(
            int((20.0f - world.y) / 40.0f * kSize.y), 0, kSize.y - 1);
        const uint8_t* p = &pixels[(std::size_t(y) * kSize.x + x) * 4];
        return int(p[0]) + p[1] + p[2];
      };
      int lit = 0;
      for (const PointLight& light : lights)
        if (brightness(glm::vec2(light.position)) >
            brightness(glm::vec2(light.position) + glm::vec2(1.0f)) + 30)
          ++lit;
      CHECK(lit == int(lights.size()));
      if (lit != int(lights.size()))
        std::fprintf(
            stderr, "  %d of %zu lights lit their spot\n", lit, lights.size());
    }
    harness.renderer.destroyMaterial(material);
    CHECK(reportErrors());
  }

  TEST("a surface's normal should lean the way its texture runs")
  {
    // On the plane, tangent space's +x is world +x and up the image is world
    // +y. The default sun is toward -x and -y, so leaning that way is brighter.
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    const auto brightness = [&](glm::vec3 normal)
    {
      graph::Graph g;
      const graph::NodeId out = g.add(std::string(graph::kOutputType), {});
      g.find(out)->inputs[graph::kNormal] = glm::vec4(normal, 0.0f);
      g.find(out)->inputs[graph::kBaseColor] =
          glm::vec4(0.3f); // not lit to white
      const MaterialHandle handle =
          harness.renderer.createMaterial(graphMaterial(g, AlphaMode::Opaque));
      const std::vector<uint8_t> pixels =
          drawPlane(harness, plane, handle, 0.0f);
      harness.renderer.destroyMaterial(handle);
      if (pixels.empty())
        return -1.0f;
      const std::size_t middle =
          (std::size_t(kSize.y / 2) * kSize.x + kSize.x / 2) * 4;
      return float(pixels[middle]) + pixels[middle + 1] + pixels[middle + 2];
    };
    // Lighting tops out at full (so leaning toward the sun can't beat flat):
    // compare each lean with its opposite.
    CHECK(brightness({-0.6f, 0.0f, 0.8f}) > brightness({0.6f, 0.0f, 0.8f}));
    CHECK(brightness({0.0f, -0.6f, 0.8f}) > brightness({0.0f, 0.6f, 0.8f}));
    CHECK(brightness({0.6f, 0.0f, 0.8f}) < brightness({0.0f, 0.0f, 1.0f}));
    CHECK(reportErrors());
  }

  TEST("a mesh handle kept after its mesh is destroyed should find nothing, "
       "even once another mesh has its place")
  {
    const MeshHandle old =
        harness.renderer.createMesh(meshes::plane({1.0f, 1.0f}));
    CHECK(old.valid());
    harness.renderer.destroyMesh(old);
    const MeshHandle next =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    CHECK(next.valid());
    CHECK(next != old);
    CHECK(!harness.renderer.hasMesh(old));
    CHECK(harness.renderer.hasMesh(next));
    harness.renderer.destroyMesh(old); // stale: leaves `next` alone
    CHECK(harness.renderer.hasMesh(next));
    harness.renderer.destroyMesh(next);
    CHECK(reportErrors());
  }

  TEST("a texture bigger than the device allows should be drawn smaller, not "
       "fail")
  {
    auto wide = std::make_shared<TextureData>();
    wide->width = context.maxTextureSize() * 2;
    wide->height = 2;
    wide->rgba.assign(std::size_t(wide->width) * wide->height * 4, 200);
    MeshMaterial material;
    material.baseTexture = wide;
    const MaterialHandle handle = harness.renderer.createMaterial(material);
    CHECK(handle.valid());
    harness.draw(handle);
    harness.renderer.destroyMaterial(handle);
    CHECK(reportErrors());
  }

  // --- a physical sky and cascaded shadows ---------------------------------

  // A pixel's rgb (x across, y down).
  const auto pixelAt = [](const std::vector<uint8_t>& pixels, int x, int y)
  {
    if (pixels.empty())
      return glm::ivec3(-1);
    const std::size_t i = (std::size_t(y) * kSize.x + x) * 4;
    return glm::ivec3(pixels[i], pixels[i + 1], pixels[i + 2]);
  };
  const auto sunAt = [](float degrees, float azimuth = 0.0f)
  {
    const float e = glm::radians(degrees);
    const float a = glm::radians(azimuth);
    return glm::vec3(
        std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e));
  };
  // A frame's physical sky, its sun toward `toSun`.
  uint64_t skyRevision = 0;
  const auto physicalSky = [&](FrameParams& frame, const glm::vec3& toSun)
  {
    SkySettings settings;
    settings.model = SkyModel::Physical;
    static const auto model = std::make_shared<const PhysicalSky>(settings);
    frame.sky.physical = model;
    frame.sky.toSun = glm::normalize(toSun);
    frame.sky.sunLight = model->sunLight(toSun);
    frame.sky.irradiance = model->irradiance(toSun);
    frame.sky.groundLight = model->groundRadiance(toSun);
    frame.sky.revision = ++skyRevision;
    frame.lighting.sun.direction = frame.sky.toSun;
    // Exposed as a photo of a sunny day would be (the test's target isn't
    // sRGB, so mid-grey would be dark).
    frame.grading.toneMapper = ToneMapper::Aces;
    frame.grading.exposure = 1.5f;
    frame.bloom.enabled = false;
  };
  const MeshHandle skyPlane =
      harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
  MeshMaterial matte;
  matte.baseColor = {0.6f, 0.6f, 0.6f, 1.0f};
  matte.roughness = 1.0f;
  const MaterialHandle matteMaterial = harness.renderer.createMaterial(matte);

  TEST("a physical sky should be drawn behind everything: blue overhead by "
       "day, warm toward a setting sun, dark at night")
  {
    // Nothing drawn (the plane scaled away); a camera looking at the sky.
    MeshInstance nothing;
    nothing.model = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
    const auto look = [&](const glm::vec3& toSun, const glm::vec3& at)
    {
      return drawPlane(
          harness,
          skyPlane,
          matteMaterial,
          0.0f,
          [&](FrameParams& frame)
          {
            physicalSky(frame, toSun);
            frame.cameraPosition = {0.0f, 0.0f, 1.7f};
            frame.view = glm::lookAt(frame.cameraPosition,
                                     frame.cameraPosition + at,
                                     glm::vec3(0.0f, 0.0f, 1.0f));
            frame.projection = glm::perspectiveRH_ZO(1.0f, 1.0f, 0.1f, 100.0f);
            frame.viewProjection = frame.projection * frame.view;
            frame.cameraForward = glm::normalize(at);
          },
          nothing);
    };
    const glm::vec3 upward = glm::normalize(glm::vec3(0.0f, 1.0f, 2.0f));
    const glm::ivec3 day = pixelAt(look(sunAt(50.0f, 180.0f), upward), 32, 32);
    CHECK(day.b > day.r + 30 && day.b > 60);

    // Toward a setting sun, along the horizon (just above it).
    const glm::vec3 west = sunAt(2.0f, 90.0f);
    const std::vector<uint8_t> dusk =
        look(west, glm::normalize(glm::vec3(0.0f, 1.0f, 0.12f)));
    const glm::ivec3 glow = pixelAt(dusk, 32, 40);
    CHECK(glow.r > glow.b + 30);

    const glm::ivec3 night = pixelAt(look(sunAt(-20.0f), upward), 32, 32);
    CHECK(night.r >= 0 && night.r + night.g + night.b < 40);
    CHECK(night.b >= night.r);
    if (day.b <= day.r + 30 || glow.r <= glow.b + 30 ||
        night.r + night.g + night.b >= 40)
      std::printf("    (sky: day %d %d %d, dusk %d %d %d, night %d %d %d)\n",
                  day.r,
                  day.g,
                  day.b,
                  glow.r,
                  glow.g,
                  glow.b,
                  night.r,
                  night.g,
                  night.b);
    CHECK(reportErrors());
  }

  TEST("a physical sky should light the scene: the sun by day, the sky's blue "
       "in its place at night, its reflections on metal")
  {
    const auto lit = [&](const glm::vec3& toSun, MaterialHandle material)
    {
      return pixelAt(
          drawPlane(harness,
                    skyPlane,
                    material,
                    0.0f,
                    [&](FrameParams& frame) { physicalSky(frame, toSun); }),
          32,
          32);
    };
    const glm::ivec3 noon = lit(sunAt(70.0f), matteMaterial);
    const glm::ivec3 dusk = lit(sunAt(3.0f), matteMaterial);
    const glm::ivec3 night = lit(sunAt(-20.0f), matteMaterial);
    CHECK(noon.g > 120);
    CHECK(dusk.g < noon.g - 40);
    CHECK(night.g < dusk.g && night.b >= night.r);

    // Smooth metal, seen from straight above, reflects the sky overhead.
    MeshMaterial chrome;
    chrome.baseColor = {0.95f, 0.95f, 0.95f, 1.0f};
    chrome.metallic = 1.0f;
    chrome.roughness = 0.05f;
    const MaterialHandle chromeMaterial =
        harness.renderer.createMaterial(chrome);
    const glm::ivec3 mirror = lit(sunAt(40.0f, 180.0f), chromeMaterial);
    CHECK(mirror.b > mirror.r + 20);
    harness.renderer.destroyMaterial(chromeMaterial);
    if (noon.g <= 120 || mirror.b <= mirror.r + 20)
      std::printf("    (lit: noon %d %d %d, dusk %d, night %d, mirror %d %d "
                  "%d)\n",
                  noon.r,
                  noon.g,
                  noon.b,
                  dusk.g,
                  night.g,
                  mirror.r,
                  mirror.g,
                  mirror.b);
    CHECK(reportErrors());
  }

  // A small sphere above the plane's right half, shadowing its left half
  // under a sun from +X, 45 degrees up.
  const MeshHandle blocker =
      harness.renderer.createMesh(meshes::sphere(0.25f, 12, 8));
  MeshInstance blockerAt;
  blockerAt.model =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.4f, 0.0f, 0.8f));
  const MeshInstance blockers[] = {blockerAt};
  MeshBatch blockerBatch;
  blockerBatch.mesh = blocker;
  blockerBatch.instances = blockers;
  blockerBatch.shadowInstances = blockers;
  MeshInstance groundInstance;
  const MeshInstance grounds[] = {groundInstance};
  const glm::vec3 lowSun = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
  const auto shadowed = [&](uint32_t cascades, bool physical)
  {
    MeshBatch ground;
    ground.mesh = skyPlane;
    const MaterialHandle materials[] = {matteMaterial};
    ground.materials = materials;
    ground.instances = grounds;
    ground.shadowInstances = grounds;
    const MeshBatch batches[] = {blockerBatch, ground};
    MeshInstance nothing;
    nothing.model = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
    return drawPlane(
        harness,
        skyPlane,
        matteMaterial,
        0.0f,
        [&](FrameParams& frame)
        {
          if (physical)
            physicalSky(frame, lowSun);
          frame.bloom.enabled = false;
          frame.lighting.sun.direction = lowSun;
          frame.shadows.enabled = true;
          frame.shadows.softness = 1.0f;
          frame.shadows.distance = 3.0f;
          frame.lightViewProjection =
              fitSunShadow(lowSun, glm::vec3(0.0f), 3.0f, 256);
          if (cascades > 1)
          {
            CascadeView view;
            view.position = frame.cameraPosition;
            view.forward = frame.cameraForward;
            view.perspective = false;
            view.halfExtent = {1.0f, 1.0f};
            view.nearPlane = 0.1f;
            view.farPlane = 10.0f;
            frame.cascades =
                fitSunCascades(lowSun, view, cascades, 10.0f, 0.5f, 256);
            frame.shadows.cascades = cascades;
          }
        },
        nothing,
        batches);
  };

  TEST("cascaded shadows should fall where the one map's do, and a frame "
       "without them should be exactly as before")
  {
    const std::vector<uint8_t> single = shadowed(1, false);
    const std::vector<uint8_t> cascaded = shadowed(4, false);
    // The sun from +X: the sphere at x = 0.4, 0.8 up, shadows x ~ -0.4.
    const glm::ivec3 inShadow = pixelAt(single, 19, 32);
    const glm::ivec3 inSun = pixelAt(single, 4, 10);
    CHECK(inShadow.g >= 0 && inShadow.g < inSun.g - 20);
    const glm::ivec3 cascadeShadow = pixelAt(cascaded, 19, 32);
    const glm::ivec3 cascadeSun = pixelAt(cascaded, 4, 10);
    CHECK(cascadeShadow.g < cascadeSun.g - 20);
    CHECK(std::abs(cascadeShadow.g - inShadow.g) < 12);
    CHECK(std::abs(cascadeSun.g - inSun.g) < 4);
    if (inShadow.g >= inSun.g - 20 || cascadeShadow.g >= cascadeSun.g - 20)
      std::printf("    (shadow %d sun %d; cascaded shadow %d sun %d)\n",
                  inShadow.g,
                  inSun.g,
                  cascadeShadow.g,
                  cascadeSun.g);

    // Under a physical sky, too.
    const std::vector<uint8_t> physical = shadowed(3, true);
    CHECK(pixelAt(physical, 19, 32).g < pixelAt(physical, 4, 10).g - 20);

    // Flat again, one map: every pixel as the first time (nothing of the
    // physical sky or the cascades left over).
    CHECK(shadowed(1, false) == single);
    CHECK(reportErrors());
  }
  // --- ambient occlusion, height fog and terrain ----------------------------

  // A 0.5 m box standing on the 2 x 2 plane, seen from above and to the side
  // in perspective; a grey, mostly ambient-lit scene (the sun low and weak,
  // no shadows), so what ambient occlusion takes shows.
  const MeshHandle crate =
      harness.renderer.createMesh(meshes::box({0.5f, 0.5f, 0.6f}));
  MeshInstance crateAt;
  const MeshInstance crates[] = {crateAt};
  MeshBatch crateBatch;
  crateBatch.mesh = crate;
  crateBatch.instances = crates;
  glm::mat4 cornerView(1.0f);
  const auto corner =
      [&](const AmbientOcclusionSettings& occlusion, bool physical)
  {
    const MeshBatch batches[] = {crateBatch};
    return drawPlane(
        harness,
        skyPlane,
        matteMaterial,
        0.0f,
        [&](FrameParams& frame)
        {
          if (physical)
            physicalSky(frame, sunAt(10.0f, 200.0f));
          frame.bloom.enabled = false;
          frame.lighting.sun.direction = sunAt(10.0f, 200.0f);
          frame.lighting.sun.diffuseStrength = 0.15f;
          frame.lighting.ambient = 0.8f;
          frame.cameraPosition = {2.2f, -2.2f, 2.0f};
          frame.view = glm::lookAt(frame.cameraPosition,
                                   glm::vec3(0.1f, -0.1f, 0.1f),
                                   glm::vec3(0.0f, 0.0f, 1.0f));
          frame.projection = glm::perspectiveRH_ZO(0.9f, 1.0f, 0.1f, 50.0f);
          frame.viewProjection = frame.projection * frame.view;
          frame.cameraForward = glm::normalize(glm::vec3(0.1f, -0.1f, 0.1f) -
                                               frame.cameraPosition);
          frame.nearPlane = 0.1f;
          frame.farPlane = 50.0f;
          frame.ambientOcclusion = occlusion;
          cornerView = frame.viewProjection;
        },
        {},
        batches);
  };
  // Where a world point is drawn, in the last corner view.
  const auto pixelOf = [&](const glm::vec3& world)
  {
    const glm::vec4 clip = cornerView * glm::vec4(world, 1.0f);
    return glm::ivec2(int((clip.x / clip.w * 0.5f + 0.5f) * kSize.x),
                      int((0.5f - clip.y / clip.w * 0.5f) * kSize.y));
  };

  TEST("ambient occlusion should darken the ground at the foot of a wall and "
       "not the open ground, on a flat sky and a physical one")
  {
    AmbientOcclusionSettings occlusion;
    occlusion.enabled = true;
    occlusion.radius = 0.5f;
    occlusion.intensity = 1.5f;
    occlusion.samples = 16;
    for (const bool physical : {false, true})
    {
      const std::vector<uint8_t> open = corner({}, physical);
      const std::vector<uint8_t> occluded = corner(occlusion, physical);
      // Just outside the box's +X face, on the ground; and far from it.
      const glm::ivec2 foot = pixelOf({0.29f, -0.05f, 0.0f});
      const glm::ivec2 away = pixelOf({0.85f, -0.85f, 0.0f});
      const glm::ivec3 footOpen = pixelAt(open, foot.x, foot.y);
      const glm::ivec3 footShaded = pixelAt(occluded, foot.x, foot.y);
      const glm::ivec3 awayOpen = pixelAt(open, away.x, away.y);
      const glm::ivec3 awayShaded = pixelAt(occluded, away.x, away.y);
      // (Under the physical sky, tone mapped, a little less.)
      const int darker = physical ? 5 : 10;
      CHECK(footOpen.g > 30);
      CHECK(footShaded.g < footOpen.g - darker);
      CHECK(std::abs(awayShaded.g - awayOpen.g) <= 3);
      if (footShaded.g >= footOpen.g - darker ||
          std::abs(awayShaded.g - awayOpen.g) > 3)
        std::printf("    (%s: foot %d -> %d, away %d -> %d)\n",
                    physical ? "physical" : "flat",
                    footOpen.g,
                    footShaded.g,
                    awayOpen.g,
                    awayShaded.g);
    }
    // Off again: exactly as before it was on.
    const std::vector<uint8_t> before = corner({}, false);
    corner(occlusion, false);
    CHECK(corner({}, false) == before);
    CHECK(reportErrors());
  }
  harness.renderer.destroyMesh(crate);

  TEST("height fog should hide far things in the sky's colour, thin with "
       "height, start where it starts, and take haze's place")
  {
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    MeshMaterial red;
    red.baseColor = {1.0f, 0.0f, 0.0f, 1.0f};
    const MaterialHandle material = harness.renderer.createMaterial(red);
    // The plane 5 below the camera (at 5 up); the sky blue.
    const auto draw = [&](const FogSettings& fog, float haze = 0.0f)
    {
      const std::vector<uint8_t> pixels =
          drawPlane(harness,
                    plane,
                    material,
                    0.0f,
                    [&](FrameParams& frame)
                    {
                      frame.bloom.enabled = false;
                      frame.lighting.skyColor = {0.0f, 0.0f, 1.0f};
                      frame.haze = {.strength = haze, .distance = 2.0f};
                      frame.fog = fog;
                    });
      return pixelAt(pixels, 32, 32);
    };
    const glm::ivec3 clear = draw({});
    FogSettings thick{.enabled = true, .density = 1.0f, .heightFalloff = 0.0f};
    const glm::ivec3 fogged = draw(thick);
    CHECK(clear.r > 100 && clear.b < 30);
    CHECK(fogged.r < 20 && fogged.b > 220);
    // Thinner with height: based far below, little reaches up to the plane.
    FogSettings low = thick;
    low.heightFalloff = 2.0f;
    low.baseHeight = -3.0f;
    const glm::ivec3 thin = draw(low);
    CHECK(thin.r > fogged.r + 60 && thin.b < fogged.b - 60);
    // Starting past the plane: none.
    FogSettings far = thick;
    far.startDistance = 6.0f;
    CHECK(draw(far) == clear);
    // At most half.
    FogSettings half = thick;
    half.maxOpacity = 0.5f;
    const glm::ivec3 halfway = draw(half);
    CHECK(std::abs(halfway.b - 128) < 30 && halfway.r > 60);
    // On, it takes haze's place (here, none at all).
    FogSettings none{.enabled = true, .density = 0.0f};
    const glm::ivec3 hazy = draw({}, 1.0f);
    CHECK(hazy.b > 220);
    CHECK(draw(none, 1.0f) == clear);
    harness.renderer.destroyMaterial(material);
    harness.renderer.destroyMesh(plane);
    CHECK(reportErrors());
  }

  TEST("height fog under a physical sky should glow toward the sun, and fog "
       "the sky's horizon")
  {
    MeshInstance nothing;
    nothing.model = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
    const glm::vec3 west = sunAt(4.0f, 90.0f);
    const auto look = [&](const FogSettings& fog, const glm::vec3& at)
    {
      return pixelAt(
          drawPlane(
              harness,
              skyPlane,
              matteMaterial,
              0.0f,
              [&](FrameParams& frame)
              {
                physicalSky(frame, west);
                frame.sky.physical = std::make_shared<const PhysicalSky>(
                    SkySettings{.model = SkyModel::Physical, .sunSize = 0.0f});
                frame.cameraPosition = {0.0f, 0.0f, 1.7f};
                frame.view = glm::lookAt(frame.cameraPosition,
                                         frame.cameraPosition + at,
                                         glm::vec3(0.0f, 0.0f, 1.0f));
                frame.projection =
                    glm::perspectiveRH_ZO(1.0f, 1.0f, 0.1f, 400.0f);
                frame.viewProjection = frame.projection * frame.view;
                frame.cameraForward = glm::normalize(at);
                frame.farPlane = 400.0f;
                frame.fog = fog;
              },
              nothing),
          32,
          32);
    };
    FogSettings fog{.enabled = true,
                    .density = 0.05f,
                    .heightFalloff = 0.05f,
                    .sunGlow = 1.0f};
    const glm::vec3 towardSun = glm::normalize(glm::vec3(0.0f, 1.0f, 0.05f));
    const glm::vec3 awayFromSun = glm::normalize(glm::vec3(0.0f, -1.0f, 0.05f));
    const glm::ivec3 sunward = look(fog, towardSun);
    const glm::ivec3 behind = look(fog, awayFromSun);
    const glm::ivec3 clearSunward = look({}, towardSun);
    CHECK(sunward.r > behind.r + 20);
    CHECK(sunward != clearSunward);
    if (sunward.r <= behind.r + 20)
      std::printf("    (toward the sun %d %d %d, away %d %d %d)\n",
                  sunward.r,
                  sunward.g,
                  sunward.b,
                  behind.r,
                  behind.g,
                  behind.b);
    CHECK(reportErrors());
  }

  TEST("a terrain material should blend its layers by its splat map, and show "
       "the map painted")
  {
    const MeshHandle plane =
        harness.renderer.createMesh(meshes::plane({2.0f, 2.0f}));
    const auto solid = [](uint8_t r, uint8_t g, uint8_t b)
    {
      auto image = std::make_shared<TextureData>();
      image->width = image->height = 8;
      for (int i = 0; i < 64; ++i)
        image->rgba.insert(image->rgba.end(), {r, g, b, 255});
      return image;
    };
    // The left half the first layer (red), the right the second (green).
    auto splat = blankSplat(16);
    for (int y = 0; y < 16; ++y)
      for (int x = 8; x < 16; ++x)
        splat->rgba[(std::size_t(y) * 16 + x) * 4] = 255;
    for (const bool antiTiling : {false, true})
    {
      auto terrain = std::make_shared<TerrainMaterial>();
      terrain->splat = splat;
      terrain->antiTiling = antiTiling;
      terrain->variation = 0.0f;
      terrain->layers.push_back({.baseTexture = solid(255, 0, 0)});
      terrain->layers.push_back({.baseTexture = solid(0, 255, 0)});
      MeshMaterial look;
      look.terrain = terrain;
      const MaterialHandle material = harness.renderer.createMaterial(look);
      CHECK(material.valid());
      const auto draw = [&]
      {
        return drawPlane(harness,
                         plane,
                         material,
                         0.0f,
                         [](FrameParams& frame)
                         {
                           frame.bloom.enabled = false;
                           frame.lighting.ambient = 1.0f;
                         });
      };
      const std::vector<uint8_t> pixels = draw();
      const glm::ivec3 left = pixelAt(pixels, 8, 32);
      const glm::ivec3 right = pixelAt(pixels, 56, 32);
      CHECK(left.r > 120 && left.g < 30);
      CHECK(right.g > 120 && right.r < 30);
      if (!(left.r > 120 && left.g < 30 && right.g > 120 && right.r < 30))
        std::printf("    (anti-tiling %d: left %d %d %d, right %d %d %d)\n",
                    antiTiling,
                    left.r,
                    left.g,
                    left.b,
                    right.r,
                    right.g,
                    right.b);

      // Paint the left green too: shown once updated.
      if (antiTiling)
      {
        const auto changed =
            paintSplat(*splat, {0.25f, 0.5f}, {0.3f, 0.6f}, 1, 1.0f, 0.0f);
        CHECK(changed.second.x > changed.first.x);
        harness.renderer.updateTexture(splat, changed.first, changed.second);
        const glm::ivec3 painted = pixelAt(draw(), 8, 32);
        CHECK(painted.g > 120 && painted.r < 30);
      }
      harness.renderer.destroyMaterial(material);
    }
    harness.renderer.destroyMesh(plane);
    CHECK(reportErrors());
  }

  harness.renderer.destroyMesh(blocker);
  harness.renderer.destroyMaterial(matteMaterial);
  harness.renderer.destroyMesh(skyPlane);

  return testing::report("webGpuRendererTests");
}
