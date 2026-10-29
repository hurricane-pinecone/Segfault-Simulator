#include "../../../testHarness.h"

#include <engine/core/rendering/3d/material.h>
#include <engine/core/rendering/3d/materialGraph.h>
#include <engine/core/rendering/3d/materialGraphCompiler.h>
#include <engine/core/rendering/3d/mesh.h>

#include <cmath>
#include <cstdio>
#include <memory>

using namespace sfs;
using namespace sfs::materialGraph;

namespace
{

bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

bool near(glm::vec3 a, glm::vec3 b)
{
  return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

bool hasError(const CompiledGraph& compiled, NodeId node)
{
  for (const CompileError& error : compiled.errors)
    if (error.node == node)
      return true;
  return false;
}

SurfaceValues
evaluate(const CompiledGraph& compiled,
         const SurfacePoint& point = {},
         std::vector<std::shared_ptr<const TextureData>> textures = {})
{
  return compiled.shader->evaluate(point, compiled.parameters, textures);
}

} // namespace

int main()
{
  TEST("the default graph should compile to the standard look: colour x vertex "
       "colour, matte")
  {
    const Graph graph = Graph::makeDefault();
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    CHECK(compiled.shader != nullptr);
    if (!compiled.shader)
      return testing::report("materialGraphTests");
    CHECK(compiled.shader->wgsl().find(
              "fn surface(in: SurfaceIn) -> Surface") != std::string::npos);
    CHECK(compiled.textures.empty());

    SurfacePoint point;
    point.vertexColor = {0.5f, 0.25f, 1.0f};
    const SurfaceValues surface = evaluate(compiled, point);
    CHECK(near(surface.baseColor, {0.5f, 0.25f, 1.0f}));
    CHECK(surface.alpha == 1.0f);
    CHECK(surface.metallic == 0.0f);
    CHECK(surface.roughness == 1.0f);
    CHECK(near(surface.emission, glm::vec3(0.0f)));
    CHECK(surface.alphaClip == 0.5f);
  }

  TEST("a graph should survive a round trip through JSON, in a material")
  {
    Graph graph = Graph::makeDefault();
    const NodeId texture = graph.add("texture", {-700.0f, 200.0f});
    graph.find(texture)->texture = "asset:3f55f38a39f69719fbe27c30a302af61";
    graph.find(texture)->dataTexture = true;
    const NodeId number = graph.add("number", {-500.0f, 300.0f});
    graph.find(number)->value.x = 0.25f;
    CHECK(graph.connect(number, 0, graph.output(), kRoughness));
    graph.find(graph.output())->inputs[kMetallic].x = 0.75f;

    const auto loaded = Graph::fromJson(graph.toJson());
    CHECK(loaded.has_value());
    if (loaded)
      CHECK(*loaded == graph);

    MaterialAsset material;
    material.graph = graph;
    material.alphaMode = AlphaMode::Blend;
    const auto asset = MaterialAsset::fromJson(material.toJson());
    CHECK(asset.has_value());
    if (asset)
    {
      CHECK(asset->graph.has_value());
      CHECK(asset->alphaMode == AlphaMode::Blend);
      CHECK(*asset == material);
    }
    CHECK(material.toJson()["formatVersion"] == 2);
    CHECK(!material.toJson().contains("baseColor"));
  }

  TEST("connecting should replace an input's link, and refuse loops and "
       "missing pins")
  {
    Graph graph;
    const NodeId a = graph.add("add", {});
    const NodeId b = graph.add("add", {});
    const NodeId c = graph.add("add", {});
    CHECK(graph.connect(a, 0, b, 0));
    CHECK(graph.connect(b, 0, c, 0));
    CHECK(!graph.connect(c, 0, a, 0)); // a loop
    CHECK(!graph.connect(a, 0, a, 1)); // itself
    CHECK(!graph.connect(a, 3, c, 1)); // no such output
    CHECK(!graph.connect(a, 0, c, 9)); // no such input
    CHECK(graph.feeds(a, c));
    CHECK(!graph.feeds(c, a));

    CHECK(graph.connect(c, 0, b, 1) == false); // c is fed by b
    CHECK(graph.connect(a, 0, b, 0));          // same link again: still one
    CHECK(graph.links.size() == 2);
    const NodeId d = graph.add("number", {});
    CHECK(graph.connect(d, 0, b, 0)); // replaces a's link
    CHECK(graph.linkInto(b, 0)->fromNode == d);
    CHECK(graph.links.size() == 2);

    graph.disconnect(b, 0);
    CHECK(graph.linkInto(b, 0) == nullptr);
    graph.remove(b);
    CHECK(graph.links.empty());
    CHECK(graph.find(b) == nullptr);
    CHECK(graph.add("sparkles", {}) == 0);
  }

  TEST("a dynamic node should take the widest type connected to it")
  {
    Graph graph;
    const NodeId output = graph.add(std::string(kOutputType), {});
    const NodeId color = graph.add("color", {});
    const NodeId number = graph.add("number", {});
    const NodeId multiply = graph.add("multiply", {});
    graph.find(color)->value = {0.5f, 1.0f, 0.25f, 1.0f};
    graph.find(number)->value.x = 2.0f;
    graph.connect(color, 0, multiply, 0);  // Vector 3
    graph.connect(number, 0, multiply, 1); // a number, spread over all three
    graph.connect(multiply, 0, output, kBaseColor);

    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    CHECK(compiled.dynamicTypes.at(multiply) == ValueType::Vec3);
    CHECK(compiled.dynamicTypes.at(color) ==
          ValueType::Float); // no dynamic inputs
    if (compiled.shader)
      CHECK(near(evaluate(compiled).baseColor, {1.0f, 2.0f, 0.5f}));
  }

  TEST("values should only change parameters; links should change the shader")
  {
    Graph graph = Graph::makeDefault();
    const CompiledGraph before = compile(graph);
    for (Node& node : graph.nodes)
      if (node.type == "color")
        node.value = {0.1f, 0.2f, 0.3f, 1.0f};
    graph.find(graph.output())->inputs[kRoughness].x = 0.2f;
    const CompiledGraph valuesChanged = compile(graph);
    CHECK(before.shader->wgsl() == valuesChanged.shader->wgsl());
    CHECK(before.parameters != valuesChanged.parameters);

    const NodeId number = graph.add("number", {});
    graph.connect(number, 0, graph.output(), kMetallic);
    CHECK(compile(graph).shader->wgsl() != before.shader->wgsl());
  }

  TEST("compiling should report a missing or second output, and unknown nodes "
       "that feed it")
  {
    CHECK(hasError(compile(Graph{}), 0));

    Graph twoOutputs = Graph::makeDefault();
    const NodeId second = twoOutputs.add(std::string(kOutputType), {});
    const CompiledGraph compiled = compile(twoOutputs);
    CHECK(!compiled.shader);
    CHECK(hasError(compiled, second));

    // An unknown node is fine until it feeds the output.
    nlohmann::json json = Graph::makeDefault().toJson();
    json["nodes"].push_back(
        {{"id", 50}, {"type", "sparkles"}, {"position", {0, 0}}});
    const auto withUnknown = Graph::fromJson(json);
    CHECK(withUnknown.has_value());
    if (withUnknown)
    {
      CHECK(withUnknown->find(50) != nullptr);
      CHECK(compile(*withUnknown).errors.empty());
    }

    Graph feedingUnknown = Graph::makeDefault();
    feedingUnknown.nodes.push_back(
        {.id = feedingUnknown.nextId++, .type = "sparkles"});
    feedingUnknown.links.push_back(
        {feedingUnknown.nodes.back().id, 0, feedingUnknown.output(), kAlpha});
    CHECK(hasError(compile(feedingUnknown), feedingUnknown.nodes.back().id));
  }

  TEST("loading should drop links to missing pins, second links into an input, "
       "and loops")
  {
    const nlohmann::json json = {
        {"nodes",
         {{{"id", 1}, {"type", "output"}},
          {{"id", 2}, {"type", "add"}},
          {{"id", 3}, {"type", "add"}},
          {{"id", 3}, {"type", "number"}}, // a repeated id
          {{"id", 0}, {"type", "number"}}}},
        {"links",
         {{{"from", {2, "Out"}}, {"to", {1, "Alpha"}}},
          {{"from", {3, "Out"}}, {"to", {1, "Alpha"}}}, // Alpha has a link
          {{"from", {2, "Out"}}, {"to", {3, "A"}}},
          {{"from", {3, "Out"}}, {"to", {2, "A"}}}, // a loop
          {{"from", {2, "Nope"}}, {"to", {1, "Metallic"}}},
          {{"from", {2, "Out"}}, {"to", 7}}}}};
    const auto graph = Graph::fromJson(json);
    CHECK(graph.has_value());
    if (!graph)
      return testing::report("materialGraphTests");
    CHECK(graph->nodes.size() == 3);
    CHECK(graph->links.size() == 2);
    CHECK(graph->nextId == 4);
    CHECK(graph->linkInto(1, kAlpha)->fromNode == 2);
    CHECK(!Graph::fromJson(nlohmann::json::array()).has_value());
  }

  TEST("math and vector nodes should work out what their WGSL does")
  {
    // One node fed by numbers, its output into the base colour's red.
    const auto run =
        [](const char* type, std::vector<float> inputs, uint32_t output = 0)
    {
      Graph graph;
      const NodeId out = graph.add(std::string(kOutputType), {});
      const NodeId node = graph.add(type, {});
      for (std::size_t i = 0; i < inputs.size(); ++i)
        graph.find(node)->inputs[i] = glm::vec4(inputs[i]);
      graph.connect(node, output, out, kAlpha);
      const CompiledGraph compiled = compile(graph);
      return compiled.shader ? evaluate(compiled).alpha : NAN;
    };
    CHECK(near(run("add", {2, 3}), 5.0f));
    CHECK(near(run("subtract", {2, 3}), -1.0f));
    CHECK(near(run("multiply", {2, 3}), 6.0f));
    CHECK(near(run("divide", {3, 2}), 1.5f));
    CHECK(near(run("power", {2, 3}), 8.0f));
    CHECK(near(run("lerp", {2, 4, 0.25f}), 2.5f));
    CHECK(near(run("clamp", {5, 0, 1}), 1.0f));
    CHECK(near(run("saturate", {-1}), 0.0f));
    CHECK(near(run("oneMinus", {0.25f}), 0.75f));
    CHECK(near(run("fraction", {-1.25f}), 0.75f));
    CHECK(near(run("step", {0.5f, 0.4f}), 0.0f));
    CHECK(near(run("step", {0.5f, 0.5f}), 1.0f));
    CHECK(near(run("smoothstep", {0, 1, 0.5f}), 0.5f));
    CHECK(near(run("remap", {5, 0, 10, 100, 200}), 150.0f));
    CHECK(near(
        run("combine", {1, 2, 3, 4}), 1.0f)); // RGBA -> its first component
    CHECK(near(run("split", {7}, 1), 0.0f));  // a number has no G

    // A Vector 3 through Split, Length and Dot.
    Graph graph;
    const NodeId out = graph.add(std::string(kOutputType), {});
    const NodeId combine = graph.add("combine", {});
    graph.find(combine)->inputs = {
        glm::vec4(3), glm::vec4(4), glm::vec4(12), glm::vec4(1)};
    const NodeId length = graph.add("length", {});
    const NodeId split = graph.add("split", {});
    graph.connect(combine, 1, length, 0); // RGB
    graph.connect(combine, 1, split, 0);
    graph.connect(length, 0, out, kAlpha);
    graph.connect(split, 2, out, kMetallic); // B
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    if (compiled.shader)
    {
      CHECK(near(evaluate(compiled).alpha, 13.0f));
      CHECK(near(evaluate(compiled).metallic, 12.0f));
    }
  }

  TEST("inputs with nothing connected should read the mesh's uv, normal and "
       "view direction")
  {
    Graph graph;
    const NodeId out = graph.add(std::string(kOutputType), {});
    const NodeId tiling = graph.add("tilingOffset", {});
    graph.find(tiling)->inputs[1] = {2.0f, 3.0f, 0.0f, 0.0f};
    graph.find(tiling)->inputs[2] = {0.5f, 0.0f, 0.0f, 0.0f};
    const NodeId split = graph.add("split", {});
    const NodeId fresnel = graph.add("fresnel", {});
    graph.connect(tiling, 0, split, 0);
    graph.connect(split, 0, out, kAlpha);
    graph.connect(split, 1, out, kMetallic);
    graph.connect(fresnel, 0, out, kRoughness);

    SurfacePoint point;
    point.uv = {0.25f, 0.5f};
    point.normal = {0.0f, 0.0f, 1.0f};
    point.toView = {0.0f, 0.0f, 1.0f}; // looking straight at it: no rim
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    if (!compiled.shader)
      return testing::report("materialGraphTests");
    SurfaceValues surface = evaluate(compiled, point);
    CHECK(near(surface.alpha, 1.0f));    // 0.25 x 2 + 0.5
    CHECK(near(surface.metallic, 1.5f)); // 0.5 x 3
    CHECK(near(surface.roughness, 0.0f));
    point.toView = {1.0f, 0.0f, 0.0f}; // edge on: full rim
    CHECK(near(evaluate(compiled, point).roughness, 1.0f));
  }

  TEST("a texture node should read its texture, white without one")
  {
    Graph graph;
    const NodeId out = graph.add(std::string(kOutputType), {});
    const NodeId texture = graph.add("texture", {});
    graph.connect(texture, 1, out, kBaseColor); // RGB
    graph.connect(texture, 5, out, kAlpha);     // A
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.textures.size() == 1);
    CHECK(compiled.shader && compiled.shader->textureCount() == 1);
    if (!compiled.shader)
      return testing::report("materialGraphTests");
    CHECK(compiled.shader->wgsl().find("var graphTexture0: texture_2d<f32>") !=
          std::string::npos);
    CHECK(near(evaluate(compiled).baseColor, glm::vec3(1.0f)));

    // 2x1, nearest: red then (data) half grey, half transparent.
    auto image = std::make_shared<TextureData>();
    image->width = 2;
    image->height = 1;
    image->rgba = {255, 0, 0, 255, 128, 128, 128, 128};
    image->nearest = true;
    image->linear = true;
    SurfacePoint point;
    point.uv = {0.25f, 0.5f};
    CHECK(
        near(evaluate(compiled, point, {image}).baseColor, {1.0f, 0.0f, 0.0f}));
    point.uv = {1.75f, 0.5f}; // repeats
    const SurfaceValues right = evaluate(compiled, point, {image});
    CHECK(near(right.baseColor, glm::vec3(128.0f / 255.0f)));
    CHECK(near(right.alpha, 128.0f / 255.0f));
  }

  TEST("noise should repeat exactly and stay between 0 and 1")
  {
    Graph graph;
    const NodeId out = graph.add(std::string(kOutputType), {});
    const NodeId noise = graph.add("noise", {});
    graph.connect(noise, 0, out, kAlpha);
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    if (!compiled.shader)
      return testing::report("materialGraphTests");
    CHECK(compiled.shader->wgsl().find("fn noiseGradientNoise") !=
          std::string::npos);
    float low = 1.0f, high = 0.0f;
    bool repeats = true;
    for (int i = 0; i < 400; ++i)
    {
      SurfacePoint point;
      point.uv = {i * 0.0137f - 2.0f, i * 0.0071f + 1.0f};
      const float value = evaluate(compiled, point).alpha;
      repeats = repeats && value == evaluate(compiled, point).alpha;
      low = std::min(low, value);
      high = std::max(high, value);
    }
    CHECK(repeats);
    CHECK(low >= 0.0f);
    CHECK(high <= 1.0f);
    CHECK(high - low > 0.3f); // it does vary
  }

  TEST("a standard material as a graph should look the same")
  {
    auto base = std::make_shared<TextureData>();
    base->width = base->height = 2;
    base->rgba = {
        255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 255, 255, 255, 255, 0};
    auto data = std::make_shared<TextureData>();
    data->width = data->height = 2;
    data->rgba = {
        0, 64, 255, 255, 0, 255, 0, 255, 0, 128, 128, 255, 0, 0, 64, 255};
    data->linear = true;
    auto bumps = std::make_shared<TextureData>();
    bumps->width = bumps->height = 2;
    bumps->rgba = {200,
                   128,
                   230,
                   255,
                   100,
                   160,
                   220,
                   255,
                   128,
                   128,
                   255,
                   255,
                   60,
                   90,
                   200,
                   255};
    bumps->linear = true;

    for (const bool textured : {false, true})
    {
      MaterialAsset standard;
      standard.baseColor = {0.5f, 0.75f, 1.0f, 0.5f};
      standard.metallic = 0.8f;
      standard.roughness = 0.6f;
      standard.alphaCutoff = 0.3f;
      standard.tiling = {2.0f, 3.0f};
      standard.offset = {0.25f, 0.5f};
      if (textured)
      {
        standard.baseTexture = "asset:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        standard.metallicRoughnessTexture =
            "asset:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        standard.normalTexture = "asset:cccccccccccccccccccccccccccccccc";
        standard.normalScale = 0.7f;
      }
      const CompiledGraph compiled = compile(standard.toGraph());
      CHECK(compiled.errors.empty());
      if (!compiled.shader)
        continue;
      // The graph's textures, in its order.
      std::vector<std::shared_ptr<const TextureData>> textures;
      for (const TextureSlot& slot : compiled.textures)
        textures.push_back(slot.texture == standard.baseTexture     ? base
                           : slot.texture == standard.normalTexture ? bumps
                                                                    : data);
      CHECK(textures.size() == (textured ? 3u : 0u));

      bool same = true;
      for (int i = 0; i < 50; ++i)
      {
        SurfacePoint point;
        point.uv = {i * 0.037f, i * 0.091f};
        point.vertexColor = {0.9f, 0.5f - i * 0.004f, 0.2f};
        const SurfaceValues graph = evaluate(compiled, point, textures);

        // The standard surface (mesh3DStandardSurface.wgsl).
        const glm::vec2 uv = point.uv * standard.tiling + standard.offset;
        glm::vec4 albedo =
            glm::vec4(point.vertexColor, 1.0f) * standard.baseColor;
        float metallic = standard.metallic, roughness = standard.roughness;
        glm::vec3 normal(0.0f, 0.0f, 1.0f);
        if (textured)
        {
          albedo *= sampleTexture(base.get(), uv);
          const glm::vec4 texel = sampleTexture(data.get(), uv);
          metallic *= texel.b;
          roughness *= texel.g;
          const glm::vec3 bump =
              glm::vec3(sampleTexture(bumps.get(), uv)) * 2.0f - 1.0f;
          normal =
              glm::normalize(glm::vec3(glm::vec2(bump) * standard.normalScale,
                                       std::max(bump.z, 0.0001f)));
        }
        same = same && near(graph.baseColor, glm::vec3(albedo)) &&
               near(graph.alpha, albedo.a) && near(graph.metallic, metallic) &&
               near(graph.roughness, roughness) &&
               near(graph.alphaClip, standard.alphaCutoff) &&
               near(graph.emission, glm::vec3(0.0f)) &&
               near(graph.normal, normal);
      }
      CHECK(same);
    }
  }

  TEST("texture directions should run right along +u and up the image")
  {
    // UV (0, 0) is the image's top left: up the image is -v.
    const glm::vec3 positions[3] = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
    const glm::vec2 uvs[3] = {{0, 1}, {1, 1}, {0, 0}};
    glm::vec3 tangent, bitangent;
    textureDirections(positions, uvs, tangent, bitangent);
    CHECK(near(tangent, {2, 0, 0}));
    CHECK(near(bitangent, {0, 2, 0}));

    const glm::vec2 flat[3] = {{0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}};
    textureDirections(positions, flat, tangent, bitangent);
    CHECK(near(tangent, glm::vec3(0.0f)));

    SurfacePoint point;
    point.normal = {0, 0, 1};
    tangentFrame(
        point.normal, {2, 0, 0}, {0, 2, 0}, point.tangent, point.bitangent);
    CHECK(near(point.tangent, {1, 0, 0}));
    CHECK(near(point.bitangent, {0, 1, 0}));
    CHECK(near(perturbNormal(point, {0, 0, 1}), {0, 0, 1}));
    CHECK(near(perturbNormal(point, {1, 0, 0}), {1, 0, 0}));
    CHECK(near(perturbNormal(point, {0, 1, 0}), {0, 1, 0}));
  }

  TEST("a tangent frame should follow the texture, mirrored with it, or the "
       "world's axes without one")
  {
    glm::vec3 tangent, bitangent;
    // Skewed texture directions: the tangent follows +u exactly; the
    // bitangent stays at right angles.
    tangentFrame({0, 0, 1}, {1, 0, 0.5f}, {0.3f, 1, 0}, tangent, bitangent);
    CHECK(near(tangent, {1, 0, 0}));
    CHECK(near(bitangent, {0, 1, 0}));
    // A mirrored texture (up the image along -y).
    tangentFrame({0, 0, 1}, {1, 0, 0}, {0, -1, 0}, tangent, bitangent);
    CHECK(near(bitangent, {0, -1, 0}));
    // No UVs: on the ground, +x and +y; on a wall, up the wall.
    tangentFrame(
        {0, 0, 1}, glm::vec3(0.0f), glm::vec3(0.0f), tangent, bitangent);
    CHECK(near(tangent, {1, 0, 0}));
    CHECK(near(bitangent, {0, 1, 0}));
    tangentFrame(
        {1, 0, 0}, glm::vec3(0.0f), glm::vec3(0.0f), tangent, bitangent);
    CHECK(near(bitangent, {0, 0, 1}));
    CHECK(std::abs(glm::dot(tangent, glm::vec3(1, 0, 0))) < 1e-5f);
  }

  TEST(
      "a triplanar normal map should bump the way each projection's image runs")
  {
    Graph graph;
    const NodeId out = graph.add(kOutputType, {});
    const NodeId bumps = graph.add("triplanarNormalMap", {});
    graph.connect(bumps, 0, out, kNormal);
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    if (!compiled.shader)
      return testing::report("materialGraphTests");
    CHECK(compiled.textures.size() == 1 && compiled.textures[0].data);

    SurfacePoint ground; // facing up, frame +x, +y
    ground.position = {0.3f, 0.6f, 0.0f};
    CHECK(near(evaluate(compiled, ground).normal, {0, 0, 1})); // no image: flat

    // Leaning along the image's +u: on the ground (read across x, y), +x.
    auto image = std::make_shared<TextureData>();
    image->width = image->height = 1;
    image->rgba = {230, 128, 128, 255};
    image->linear = true;
    const glm::vec3 leaning = evaluate(compiled, ground, {image}).normal;
    CHECK(leaning.x > 0.5f);
    CHECK(std::abs(leaning.y) < 0.02f);

    // On a wall facing +x (read across y, z), its frame from the world's axes.
    SurfacePoint wall;
    wall.normal = {1, 0, 0};
    tangentFrame(wall.normal,
                 glm::vec3(0.0f),
                 glm::vec3(0.0f),
                 wall.tangent,
                 wall.bitangent);
    wall.position = {0.0f, 0.4f, 0.7f};
    const glm::vec3 tangentSpace = evaluate(compiled, wall, {image}).normal;
    // Back in world space it leans along +y (the image's +u on that plane).
    CHECK(perturbNormal(wall, tangentSpace).y > 0.5f);
  }

  TEST("a normal map should be flat without an image, and tilt with one")
  {
    Graph graph;
    const NodeId out = graph.add(kOutputType, {});
    const NodeId normalMap = graph.add("normalMap", {});
    graph.connect(normalMap, 0, out, kNormal);
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    if (!compiled.shader)
      return testing::report("materialGraphTests");
    CHECK(compiled.textures.size() == 1 &&
          compiled.textures[0].data); // always data
    CHECK(compiled.shader->missingTexture(0) == kFlatNormal);
    CHECK(near(evaluate(compiled).normal, {0, 0, 1}));

    // Leaning right: x = +1, z = 0 -> straight along the tangent.
    auto image = std::make_shared<TextureData>();
    image->width = image->height = 1;
    image->rgba = {255, 128, 128, 255};
    image->linear = true;
    const glm::vec3 leaning = evaluate(compiled, {}, {image}).normal;
    CHECK(leaning.x > 0.99f);
    // Strength 0 flattens it.
    graph.find(normalMap)->inputs[1].x = 0.0f;
    CHECK(near(evaluate(compile(graph), {}, {image}).normal, {0, 0, 1}));
  }

  TEST("pattern nodes should repeat exactly and stay in range")
  {
    const auto run = [](const char* type,
                        uint32_t output,
                        SurfacePoint point,
                        std::vector<float> inputs = {})
    {
      Graph graph;
      const NodeId out = graph.add(kOutputType, {});
      const NodeId node = graph.add(type, {});
      for (std::size_t i = 0; i < inputs.size(); ++i)
        graph.find(node)->inputs[i + 1] = glm::vec4(inputs[i]); // after UV
      graph.connect(node, output, out, kBaseColor);
      const CompiledGraph compiled = compile(graph);
      return compiled.shader ? evaluate(compiled, point).baseColor
                             : glm::vec3(NAN);
    };
    SurfacePoint a, b;
    a.uv = {0.05f, 0.05f};
    b.uv = {0.2f, 0.05f};
    // Checkerboard: neighbouring squares take the two colours.
    CHECK(near(run("checkerboard", 0, a), glm::vec3(0.15f)));
    CHECK(near(run("checkerboard", 0, b), glm::vec3(0.85f)));

    bool inRange = true, repeats = true;
    for (int i = 0; i < 200; ++i)
    {
      SurfacePoint point;
      point.uv = {i * 0.031f - 3.0f, i * 0.017f + 0.5f};
      const float distance = run("voronoi", 0, point).x;
      const float cell = run("voronoi", 1, point).x;
      repeats = repeats && distance == run("voronoi", 0, point).x;
      inRange = inRange && distance >= 0.0f && distance < 1.5f &&
                cell >= 0.0f && cell < 1.0f;
    }
    CHECK(inRange);
    CHECK(repeats);
    // Randomness 0: a grid, each point at its square's middle.
    SurfacePoint middle;
    middle.uv = {0.1f, 0.1f}; // x 5 = (0.5, 0.5)
    CHECK(near(run("voronoi", 0, middle, {5.0f, 0.0f}).x, 0.0f));
  }

  TEST("triplanar should read the image along the axis the surface faces")
  {
    Graph graph;
    const NodeId out = graph.add(kOutputType, {});
    const NodeId triplanar = graph.add("triplanar", {});
    graph.connect(triplanar, 1, out, kBaseColor);
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    if (!compiled.shader)
      return testing::report("materialGraphTests");
    // 2x1: black left half, white right half.
    auto image = std::make_shared<TextureData>();
    image->width = 2;
    image->height = 1;
    image->rgba = {0, 0, 0, 255, 255, 255, 255, 255};
    image->nearest = image->linear = true;
    SurfacePoint top;
    top.normal = {0, 0, 1}; // reads x, y
    top.position = {0.75f, 0.0f, 0.25f};
    CHECK(near(evaluate(compiled, top, {image}).baseColor, glm::vec3(1.0f)));
    top.position = {0.25f, 0.0f, 0.75f};
    CHECK(near(evaluate(compiled, top, {image}).baseColor, glm::vec3(0.0f)));
    SurfacePoint side;
    side.normal = {1, 0, 0}; // reads y, z
    side.position = {0.25f, 0.75f, 0.0f};
    CHECK(near(evaluate(compiled, side, {image}).baseColor, glm::vec3(1.0f)));
  }

  TEST("posterize should round down to even steps")
  {
    Graph graph;
    const NodeId out = graph.add(kOutputType, {});
    const NodeId posterize = graph.add("posterize", {});
    graph.find(posterize)->inputs[0] = glm::vec4(0.6f);
    graph.connect(posterize, 0, out, kAlpha);
    CHECK(near(evaluate(compile(graph)).alpha, 0.5f));
  }

  TEST("every node type should compile feeding the output")
  {
    for (const NodeType& type : nodeTypes())
    {
      if (type.id == kOutputType)
        continue;
      Graph graph;
      const NodeId out = graph.add(std::string(kOutputType), {});
      const NodeId node = graph.add(type.id, {});
      CHECK(node != 0);
      for (uint32_t i = 0; i < type.outputs.size(); ++i)
      {
        Graph copy = graph;
        copy.connect(node, i, out, kBaseColor);
        const CompiledGraph compiled = compile(copy);
        CHECK(compiled.errors.empty());
        CHECK(compiled.shader != nullptr);
      }
    }
  }

  TEST("a node's output should change only with the parts of the point it's "
       "said to read")
  {
    // Each node feeding the Emission: change one part of the point at a time;
    // whatever changes the glow must be among what reads() says it depends on.
    SurfacePoint base;
    base.uv = {0.3f, 0.6f};
    base.position = {1.3f, -0.7f, 2.1f};
    base.vertexColor = {0.2f, 0.5f, 0.9f};
    base.time = 1.7f;
    base.toView = glm::normalize(glm::vec3(0.3f, 0.2f, 1.0f));
    struct Change
    {
      PointInputs part;
      void (*apply)(SurfacePoint&);
    };
    const Change changes[] = {
        {PointInputs::Uv,
         [](SurfacePoint& p) {
           p.uv = {0.81f, 0.17f};
         }},
        {PointInputs::Position,
         [](SurfacePoint& p) {
           p.position = {-4.2f, 3.3f, 0.4f};
         }},
        {PointInputs::Normal,
         [](SurfacePoint& p)
         { p.normal = glm::normalize(glm::vec3(0.4f, -0.5f, 0.7f)); }},
        {PointInputs::Tangent,
         [](SurfacePoint& p)
         {
           p.tangent = {0.0f, 1.0f, 0.0f};
           p.bitangent = {-1.0f, 0.0f, 0.0f};
         }},
        {PointInputs::ToView,
         [](SurfacePoint& p)
         { p.toView = glm::normalize(glm::vec3(-0.6f, 0.1f, 0.5f)); }},
        {PointInputs::VertexColor,
         [](SurfacePoint& p) {
           p.vertexColor = {0.9f, 0.1f, 0.3f};
         }},
        {PointInputs::Time, [](SurfacePoint& p) { p.time = 4.9f; }},
    };
    for (const NodeType& type : nodeTypes())
    {
      if (type.id == kOutputType)
        continue;
      for (uint32_t i = 0; i < type.outputs.size(); ++i)
      {
        Graph graph;
        const NodeId out = graph.add(std::string(kOutputType), {});
        const NodeId node = graph.add(type.id, {});
        graph.connect(node, i, out, kEmission);
        const CompiledGraph compiled = compile(graph);
        if (!compiled.shader)
          continue;
        const PointInputs reads = compiled.shader->reads(kEmission);
        const glm::vec3 before = evaluate(compiled, base).emission;
        for (const Change& change : changes)
        {
          SurfacePoint changed = base;
          change.apply(changed);
          if (!near(evaluate(compiled, changed).emission, before) &&
              !any(reads & change.part))
          {
            CHECK(false);
            std::fprintf(stderr,
                         "  %s's output %u changes with a part it doesn't say "
                         "it reads\n",
                         type.id.c_str(),
                         i);
          }
        }
      }
    }
  }

  TEST("an output should depend on what its nodes read, through every path")
  {
    Graph graph;
    const NodeId out = graph.add(std::string(kOutputType), {});
    // Emission: time -> sine, and a fresnel, added. Base colour: a number.
    const NodeId time = graph.add("time", {});
    const NodeId sine = graph.add("sine", {});
    const NodeId fresnel = graph.add("fresnel", {});
    const NodeId add = graph.add("add", {});
    const NodeId number = graph.add("number", {});
    graph.connect(time, 0, sine, 0);
    graph.connect(sine, 0, add, 0);
    graph.connect(fresnel, 0, add, 1);
    graph.connect(add, 0, out, kEmission);
    graph.connect(number, 0, out, kBaseColor);
    const CompiledGraph compiled = compile(graph);
    CHECK(compiled.errors.empty());
    const PointInputs emission = compiled.shader->reads(kEmission);
    CHECK(any(emission & PointInputs::Time));
    CHECK(any(emission & PointInputs::Normal) &&
          any(emission & PointInputs::ToView));
    CHECK(!any(emission & (PointInputs::Uv | PointInputs::Position)));
    CHECK(!any(compiled.shader->reads(kBaseColor)));
    // A value the output holds itself depends on nothing.
    CHECK(!any(compiled.shader->reads(kMetallic)));

    // One node feeding another twice is followed once, however much it's
    // shared.
    Graph shared;
    const NodeId sharedOut = shared.add(std::string(kOutputType), {});
    NodeId last = shared.add("uv", {});
    for (int i = 0; i < 40; ++i)
    {
      const NodeId next = shared.add("add", {});
      shared.connect(last, 0, next, 0);
      shared.connect(last, 0, next, 1);
      last = next;
    }
    shared.connect(last, 0, sharedOut, kEmission);
    const CompiledGraph sharedCompiled = compile(shared);
    CHECK(sharedCompiled.shader &&
          sharedCompiled.shader->reads(kEmission) == PointInputs::Uv);
  }

  return testing::report("materialGraphTests");
}
