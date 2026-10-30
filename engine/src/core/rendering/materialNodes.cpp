#include "engine/core/rendering/materialNodes.h"

#include "engine/core/rendering/mesh.h"
#include "glm/glm/common.hpp"
#include "glm/glm/exponential.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs::materialGraph
{

namespace
{

constexpr ValueType F = ValueType::Float;
constexpr ValueType V2 = ValueType::Vec2;
constexpr ValueType V3 = ValueType::Vec3;
constexpr ValueType V4 = ValueType::Vec4;

using Inputs = std::span<const std::string>;
using Outputs = std::span<glm::vec4>;

// A dynamic input with the same default in every component, so it reads the
// same at any width.
InputPin dynamicInput(std::string name, float value)
{
  return {std::move(name), std::nullopt, glm::vec4(value)};
}

InputPin
input(std::string name, ValueType type, glm::vec4 value = glm::vec4(0.0f))
{
  return {std::move(name), type, value};
}

InputPin fallbackInput(std::string name, ValueType type, Fallback fallback)
{
  return {std::move(name), type, glm::vec4(0.0f), fallback};
}

InputPin colorInput(std::string name, glm::vec4 value)
{
  return {std::move(name), V3, value, Fallback::Value, true};
}

// A number that makes sense from 0 to 1.
InputPin unitInput(std::string name, float value)
{
  return {std::move(name), F, glm::vec4(value), Fallback::Value, false, true};
}

OutputPin dynamicOutput(std::string name)
{
  return {std::move(name), std::nullopt};
}

OutputPin output(std::string name, ValueType type)
{
  return {std::move(name), type};
}

std::string call(const char* function, Inputs inputs)
{
  std::string out = std::string(function) + "(";
  for (std::size_t i = 0; i < inputs.size(); ++i)
    out += (i ? ", " : "") + inputs[i];
  return out + ")";
}

// Applies a function to every component (the dynamic math nodes work
// component by component).
template <typename Function>
glm::vec4 each(glm::vec4 a, Function f)
{
  return {f(a.x), f(a.y), f(a.z), f(a.w)};
}

template <typename Function>
glm::vec4 each(glm::vec4 a, glm::vec4 b, Function f)
{
  return {f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w)};
}

template <typename Function>
glm::vec4 each(glm::vec4 a, glm::vec4 b, glm::vec4 c, Function f)
{
  return {
      f(a.x, b.x, c.x), f(a.y, b.y, c.y), f(a.z, b.z, c.z), f(a.w, b.w, c.w)};
}

float smoothstepScalar(float low, float high, float x)
{
  // WGSL's smoothstep: undefined for low == high there, a step here.
  if (low == high)
    return x < low ? 0.0f : 1.0f;
  const float t = std::clamp((x - low) / (high - low), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// --- Noise: 2D gradient noise, the same arithmetic in WGSL and C++ (u32 maths
// wraps the same way in both), so previews match the GPU.

constexpr const char* kNoiseHelpers = R"(
fn noiseGradient(cell: vec2f) -> vec2f {
  let c = bitcast<vec2u>(vec2i(cell));
  var h = c.x * 0x8da6b343u + c.y * 0xd8163841u;
  h = h ^ (h >> 15u);
  h = h * 0x2c1b3c6du;
  h = h ^ (h >> 12u);
  h = h * 0x297a2d39u;
  h = h ^ (h >> 15u);
  let angle = f32(h >> 8u) * (6.28318530718 / 16777216.0);
  return vec2f(cos(angle), sin(angle));
}

fn noiseGradientNoise(p: vec2f) -> f32 {
  let i = floor(p);
  let f = p - i;
  let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  let a = dot(noiseGradient(i), f);
  let b = dot(noiseGradient(i + vec2f(1.0, 0.0)), f - vec2f(1.0, 0.0));
  let c = dot(noiseGradient(i + vec2f(0.0, 1.0)), f - vec2f(0.0, 1.0));
  let d = dot(noiseGradient(i + vec2f(1.0, 1.0)), f - vec2f(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 0.70710678 + 0.5;
}
)";

glm::vec2 noiseGradient(glm::vec2 cell)
{
  const uint32_t cx = static_cast<uint32_t>(static_cast<int32_t>(cell.x));
  const uint32_t cy = static_cast<uint32_t>(static_cast<int32_t>(cell.y));
  uint32_t h = cx * 0x8da6b343u + cy * 0xd8163841u;
  h = h ^ (h >> 15u);
  h = h * 0x2c1b3c6du;
  h = h ^ (h >> 12u);
  h = h * 0x297a2d39u;
  h = h ^ (h >> 15u);
  const float angle =
      static_cast<float>(h >> 8u) * (6.28318530718f / 16777216.0f);
  return {std::cos(angle), std::sin(angle)};
}

float gradientNoise(glm::vec2 p)
{
  const glm::vec2 i = glm::floor(p);
  const glm::vec2 f = p - i;
  const glm::vec2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
  const float a = glm::dot(noiseGradient(i), f);
  const float b =
      glm::dot(noiseGradient(i + glm::vec2(1, 0)), f - glm::vec2(1, 0));
  const float c =
      glm::dot(noiseGradient(i + glm::vec2(0, 1)), f - glm::vec2(0, 1));
  const float d =
      glm::dot(noiseGradient(i + glm::vec2(1, 1)), f - glm::vec2(1, 1));
  return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y) * 0.70710678f +
         0.5f;
}

// --- Voronoi: cells around random points, one per grid square, with the same
// u32 hashing in WGSL and C++.

constexpr const char* kVoronoiHelpers = R"(
fn voronoiMix(x: u32) -> u32 {
  var h = x;
  h = h ^ (h >> 16u);
  h = h * 0x7feb352du;
  h = h ^ (h >> 15u);
  h = h * 0x846ca68bu;
  h = h ^ (h >> 16u);
  return h;
}

fn voronoiRandom(cell: vec2f, salt: u32) -> f32 {
  let c = bitcast<vec2u>(vec2i(cell));
  return f32(voronoiMix(c.x * 0x8da6b343u + c.y * 0xd8163841u + salt) >> 8u) / 16777216.0;
}

fn voronoiCells(p: vec2f, randomness: f32) -> vec2f {
  let base = floor(p);
  var best = 8.0;
  var value = 0.0;
  for (var y = -1; y <= 1; y++) {
    for (var x = -1; x <= 1; x++) {
      let cell = base + vec2f(f32(x), f32(y));
      let offset = vec2f(voronoiRandom(cell, 0u), voronoiRandom(cell, 1u)) - 0.5;
      let d = distance(p, cell + 0.5 + offset * randomness);
      if (d < best) {
        best = d;
        value = voronoiRandom(cell, 2u);
      }
    }
  }
  return vec2f(best, value);
}
)";

uint32_t voronoiMix(uint32_t h)
{
  h = h ^ (h >> 16u);
  h = h * 0x7feb352du;
  h = h ^ (h >> 15u);
  h = h * 0x846ca68bu;
  h = h ^ (h >> 16u);
  return h;
}

float voronoiRandom(glm::vec2 cell, uint32_t salt)
{
  const uint32_t cx = static_cast<uint32_t>(static_cast<int32_t>(cell.x));
  const uint32_t cy = static_cast<uint32_t>(static_cast<int32_t>(cell.y));
  return static_cast<float>(
             voronoiMix(cx * 0x8da6b343u + cy * 0xd8163841u + salt) >> 8u) /
         16777216.0f;
}

glm::vec2 voronoiCells(glm::vec2 p, float randomness)
{
  const glm::vec2 base = glm::floor(p);
  float best = 8.0f, value = 0.0f;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
      const glm::vec2 cell =
          base + glm::vec2(static_cast<float>(x), static_cast<float>(y));
      const glm::vec2 offset =
          glm::vec2(voronoiRandom(cell, 0u), voronoiRandom(cell, 1u)) - 0.5f;
      const float d = glm::distance(p, cell + 0.5f + offset * randomness);
      if (d < best)
      {
        best = d;
        value = voronoiRandom(cell, 2u);
      }
    }
  return {best, value};
}

// A dynamic node whose one output is a WGSL function of its inputs, worked
// out component by component.
NodeType math(std::string id,
              std::string label,
              std::string description,
              std::vector<InputPin> inputs,
              std::vector<std::string> (*emit)(const EmitArgs&),
              void (*evaluate)(const EvaluateArgs&, Outputs))
{
  NodeType type;
  type.id = std::move(id);
  type.label = std::move(label);
  type.category = "Math";
  type.description = std::move(description);
  type.inputs = std::move(inputs);
  type.outputs = {dynamicOutput("Out")};
  type.emit = emit;
  type.evaluate = evaluate;
  return type;
}

std::vector<NodeType> makeNodeTypes()
{
  std::vector<NodeType> types;

  // --- Output
  {
    NodeType type;
    type.id = std::string(kOutputType);
    type.label = "Material Output";
    type.category = "Output";
    type.description = "What the surface looks like: every graph ends here.";
    type.inputs = {
        colorInput("Base Colour", glm::vec4(1.0f)),
        unitInput("Alpha", 1.0f),
        unitInput("Metallic", 0.0f),
        unitInput("Roughness", 1.0f),
        input("Normal", V3, glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)),
        colorInput("Emission", glm::vec4(0.0f)),
        unitInput("Alpha Clip", 0.5f),
        input("Bloom", F, glm::vec4(1.0f)),
    };
    // Read by the compiler itself.
    type.emit = [](const EmitArgs&) { return std::vector<std::string>{}; };
    type.evaluate = [](const EvaluateArgs&, Outputs) {};
    types.push_back(std::move(type));
  }

  // --- Input
  {
    NodeType type;
    type.id = "color";
    type.label = "Colour";
    type.category = "Input";
    type.description = "A colour you pick, and its alpha.";
    type.outputs = {output("Colour", V3), output("Alpha", F)};
    type.property = Property::Color;
    type.propertyDefault = glm::vec4(1.0f);
    type.emit = [](const EmitArgs& a) {
      return std::vector<std::string>{a.property + ".rgb", a.property + ".a"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      out[0] = glm::vec4(glm::vec3(a.property), 0.0f);
      out[1] = glm::vec4(a.property.a, 0.0f, 0.0f, 0.0f);
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "number";
    type.label = "Number";
    type.category = "Input";
    type.description = "A number you set.";
    type.outputs = {output("Value", F)};
    type.property = Property::Number;
    type.propertyDefault = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    type.emit = [](const EmitArgs& a)
    { return std::vector<std::string>{a.property + ".x"}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(a.property.x, 0, 0, 0); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "texture";
    type.label = "Texture";
    type.category = "Input";
    type.description = "Reads an image at the texture coordinates (the mesh's "
                       "own, unless something is "
                       "connected to UV). With no image it reads white.";
    type.inputs = {fallbackInput("UV", V2, Fallback::Uv)};
    type.outputs = {output("RGBA", V4),
                    output("RGB", V3),
                    output("R", F),
                    output("G", F),
                    output("B", F),
                    output("A", F)};
    type.property = Property::Texture;
    type.emit = [](const EmitArgs& a)
    {
      *a.body += "  let " + a.local + " = textureSample(" + a.property + ", " +
                 a.sampler + ", " + a.inputs[0] + ");\n";
      return std::vector<std::string>{a.local,
                                      a.local + ".rgb",
                                      a.local + ".r",
                                      a.local + ".g",
                                      a.local + ".b",
                                      a.local + ".a"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      const glm::vec4 texel = sampleTexture(a.texture, glm::vec2(a.inputs[0]));
      out[0] = texel;
      out[1] = glm::vec4(glm::vec3(texel), 0.0f);
      for (int i = 0; i < 4; ++i)
        out[2 + i] = glm::vec4(texel[i], 0.0f, 0.0f, 0.0f);
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "uv";
    type.reads = PointInputs::Uv;
    type.label = "UV";
    type.category = "Input";
    type.description = "The mesh's texture coordinates.";
    type.outputs = {output("UV", V2)};
    type.emit = [](const EmitArgs&)
    { return std::vector<std::string>{"in.uv"}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(a.point->uv, 0.0f, 0.0f); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "vertexColor";
    type.reads = PointInputs::VertexColor;
    type.label = "Vertex Colour";
    type.category = "Input";
    type.description = "The mesh's own colours (untextured models carry their "
                       "palette in them).";
    type.outputs = {output("Colour", V3)};
    type.emit = [](const EmitArgs&)
    { return std::vector<std::string>{"in.vertexColor"}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(a.point->vertexColor, 0.0f); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "position";
    type.reads = PointInputs::Position;
    type.label = "Position";
    type.category = "Input";
    type.description = "Where the point is in the world (Z up).";
    type.outputs = {output("Position", V3)};
    type.emit = [](const EmitArgs&)
    { return std::vector<std::string>{"in.position"}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(a.point->position, 0.0f); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "normal";
    type.reads = PointInputs::Normal;
    type.label = "Normal";
    type.category = "Input";
    type.description =
        "Which way the surface faces, in the world (unit length).";
    type.outputs = {output("Normal", V3)};
    type.emit = [](const EmitArgs&)
    { return std::vector<std::string>{"in.normal"}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(a.point->normal, 0.0f); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "viewDirection";
    type.reads = PointInputs::ToView;
    type.label = "View Direction";
    type.category = "Input";
    type.description =
        "The direction from the surface to the camera (unit length).";
    type.outputs = {output("Direction", V3)};
    type.emit = [](const EmitArgs&)
    { return std::vector<std::string>{"in.toView"}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(a.point->toView, 0.0f); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "time";
    type.reads = PointInputs::Time;
    type.label = "Time";
    type.category = "Input";
    type.description = "Seconds the scene has been running, for things that "
                       "move: water, flicker, pulsing glows.";
    type.outputs = {output("Time", F), output("Sine Time", F)};
    type.emit = [](const EmitArgs&) {
      return std::vector<std::string>{"in.time", "sin(in.time)"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      out[0] = glm::vec4(a.point->time, 0, 0, 0);
      out[1] = glm::vec4(std::sin(a.point->time), 0, 0, 0);
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "grass";
    type.reads = PointInputs::Grass;
    type.label = "Grass";
    type.category = "Input";
    type.description =
        "For a grass material: the blade's length (1 is the grass's full "
        "height), how far up it the point is (0 root, 1 tip), a random number "
        "of its own, and the grass maps' Data channels where it grows (what a "
        "game writes there, such as moisture). On anything else: 1, 0, 0 and "
        "0.";
    type.outputs = {output("Length", F),
                    output("Height", F),
                    output("Random", F),
                    output("Data", V4),
                    output("Data 0", F),
                    output("Data 1", F),
                    output("Data 2", F),
                    output("Data 3", F)};
    type.emit = [](const EmitArgs&)
    {
      return std::vector<std::string>{"in.grass.x",
                                      "in.grass.y",
                                      "in.grass.z",
                                      "in.grassData",
                                      "in.grassData.x",
                                      "in.grassData.y",
                                      "in.grassData.z",
                                      "in.grassData.w"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      for (int i = 0; i < 3; ++i)
        out[i] = glm::vec4(a.point->grass[i], 0, 0, 0);
      out[3] = a.point->grassData;
      for (int i = 0; i < 4; ++i)
        out[4 + i] = glm::vec4(a.point->grassData[i], 0, 0, 0);
    };
    types.push_back(std::move(type));
  }

  // --- Math: dynamic, component by component.
  types.push_back(math(
      "add",
      "Add",
      "A + B.",
      {dynamicInput("A", 0.0f), dynamicInput("B", 0.0f)},
      [](const EmitArgs& a)
      {
        return std::vector<std::string>{"(" + a.inputs[0] + " + " +
                                        a.inputs[1] + ")"};
      },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = a.inputs[0] + a.inputs[1]; }));
  types.push_back(math(
      "subtract",
      "Subtract",
      "A - B.",
      {dynamicInput("A", 0.0f), dynamicInput("B", 0.0f)},
      [](const EmitArgs& a)
      {
        return std::vector<std::string>{"(" + a.inputs[0] + " - " +
                                        a.inputs[1] + ")"};
      },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = a.inputs[0] - a.inputs[1]; }));
  types.push_back(math(
      "multiply",
      "Multiply",
      "A x B: darkens colours, scales numbers.",
      {dynamicInput("A", 1.0f), dynamicInput("B", 1.0f)},
      [](const EmitArgs& a)
      {
        return std::vector<std::string>{"(" + a.inputs[0] + " * " +
                                        a.inputs[1] + ")"};
      },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = a.inputs[0] * a.inputs[1]; }));
  types.push_back(math(
      "divide",
      "Divide",
      "A / B.",
      {dynamicInput("A", 1.0f), dynamicInput("B", 1.0f)},
      [](const EmitArgs& a)
      {
        return std::vector<std::string>{"(" + a.inputs[0] + " / " +
                                        a.inputs[1] + ")"};
      },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = a.inputs[0] / a.inputs[1]; }));
  types.push_back(math(
      "power",
      "Power",
      "A to the power of B.",
      {dynamicInput("A", 1.0f), dynamicInput("B", 2.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("pow", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      {
        out[0] = each(a.inputs[0],
                      a.inputs[1],
                      [](float x, float y) { return std::pow(x, y); });
      }));
  types.push_back(math(
      "minimum",
      "Minimum",
      "The smaller of A and B.",
      {dynamicInput("A", 0.0f), dynamicInput("B", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("min", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::min(a.inputs[0], a.inputs[1]); }));
  types.push_back(math(
      "maximum",
      "Maximum",
      "The larger of A and B.",
      {dynamicInput("A", 0.0f), dynamicInput("B", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("max", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::max(a.inputs[0], a.inputs[1]); }));
  types.push_back(math(
      "lerp",
      "Mix",
      "Blends from A (T = 0) to B (T = 1).",
      {dynamicInput("A", 0.0f),
       dynamicInput("B", 1.0f),
       dynamicInput("T", 0.5f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("mix", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::mix(a.inputs[0], a.inputs[1], a.inputs[2]); }));
  types.push_back(math(
      "clamp",
      "Clamp",
      "Keeps In between Min and Max.",
      {dynamicInput("In", 0.0f),
       dynamicInput("Min", 0.0f),
       dynamicInput("Max", 1.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("clamp", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      {
        // WGSL's clamp is min(max(In, Min), Max), even when Min > Max.
        out[0] = glm::min(glm::max(a.inputs[0], a.inputs[1]), a.inputs[2]);
      }));
  types.push_back(math(
      "saturate",
      "Saturate",
      "Keeps In between 0 and 1.",
      {dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("saturate", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::clamp(a.inputs[0], 0.0f, 1.0f); }));
  types.push_back(math(
      "oneMinus",
      "One Minus",
      "1 - In: inverts a colour or a mask.",
      {dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{"(1.0 - " + a.inputs[0] + ")"}; },
      [](const EvaluateArgs& a, Outputs out) { out[0] = 1.0f - a.inputs[0]; }));
  types.push_back(math(
      "negate",
      "Negate",
      "-In.",
      {dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{"(-" + a.inputs[0] + ")"}; },
      [](const EvaluateArgs& a, Outputs out) { out[0] = -a.inputs[0]; }));
  types.push_back(math(
      "absolute",
      "Absolute",
      "In without its sign.",
      {dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("abs", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::abs(a.inputs[0]); }));
  types.push_back(math(
      "fraction",
      "Fraction",
      "The part of In after the decimal point (repeats 0 to 1).",
      {dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("fract", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = a.inputs[0] - glm::floor(a.inputs[0]); }));
  types.push_back(math(
      "floor",
      "Floor",
      "In rounded down to a whole number.",
      {dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("floor", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::floor(a.inputs[0]); }));
  types.push_back(math(
      "posterize",
      "Posterize",
      "Rounds In down to Steps even levels between 0 and 1: flat bands of "
      "colour, for "
      "a toon or painted look.",
      {dynamicInput("In", 0.5f), input("Steps", F, glm::vec4(4.0f))},
      [](const EmitArgs& a)
      {
        return std::vector<std::string>{"(floor(" + a.inputs[0] + " * " +
                                        a.inputs[1] + ") / " + a.inputs[1] +
                                        ")"};
      },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::floor(a.inputs[0] * a.inputs[1].x) / a.inputs[1].x; }));
  types.push_back(math(
      "squareRoot",
      "Square Root",
      "The square root of In.",
      {dynamicInput("In", 1.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("sqrt", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::sqrt(a.inputs[0]); }));
  types.push_back(math(
      "sine",
      "Sine",
      "sin(In), In in radians: waves between -1 and 1.",
      {dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("sin", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::sin(a.inputs[0]); }));
  types.push_back(math(
      "cosine",
      "Cosine",
      "cos(In), In in radians.",
      {dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("cos", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      { out[0] = glm::cos(a.inputs[0]); }));
  types.push_back(math(
      "step",
      "Step",
      "0 where In is below Edge, 1 from Edge up: a hard mask.",
      {dynamicInput("Edge", 0.5f), dynamicInput("In", 0.0f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("step", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out)
      {
        out[0] =
            each(a.inputs[0],
                 a.inputs[1],
                 [](float edge, float x) { return x < edge ? 0.0f : 1.0f; });
      }));
  types.push_back(math(
      "smoothstep",
      "Smoothstep",
      "Rises smoothly from 0 to 1 as In goes from Edge 1 to Edge 2: a soft "
      "mask.",
      {dynamicInput("Edge 1", 0.0f),
       dynamicInput("Edge 2", 1.0f),
       dynamicInput("In", 0.5f)},
      [](const EmitArgs& a)
      { return std::vector<std::string>{call("smoothstep", a.inputs)}; },
      [](const EvaluateArgs& a, Outputs out) {
        out[0] = each(a.inputs[0], a.inputs[1], a.inputs[2], smoothstepScalar);
      }));
  types.push_back(math(
      "remap",
      "Remap",
      "Maps In from the From range to the To range.",
      {dynamicInput("In", 0.5f),
       dynamicInput("From Min", 0.0f),
       dynamicInput("From Max", 1.0f),
       dynamicInput("To Min", 0.0f),
       dynamicInput("To Max", 1.0f)},
      [](const EmitArgs& a)
      {
        const Inputs in = a.inputs;
        return std::vector<std::string>{
            "(" + in[3] + " + (" + in[0] + " - " + in[1] + ") * (" + in[4] +
            " - " + in[3] + ") / (" + in[2] + " - " + in[1] + "))"};
      },
      [](const EvaluateArgs& a, Outputs out)
      {
        const auto& in = a.inputs;
        out[0] = in[3] + (in[0] - in[1]) * (in[4] - in[3]) / (in[2] - in[1]);
      }));

  // --- Vector
  {
    NodeType type;
    type.id = "split";
    type.label = "Split";
    type.category = "Vector";
    type.description =
        "A value's components, each on its own (0 past its width).";
    type.inputs = {dynamicInput("In", 0.0f)};
    type.outputs = {
        output("R", F), output("G", F), output("B", F), output("A", F)};
    type.emit = [](const EmitArgs& a)
    {
      const std::string& in = a.inputs[0];
      const int w = width(a.dynamic);
      if (w == 1)
        return std::vector<std::string>{in, "0.0", "0.0", "0.0"};
      std::vector<std::string> out;
      for (int i = 0; i < 4; ++i)
        out.push_back(i < w ? in + "." + "rgba"[i] : std::string("0.0"));
      return out;
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      for (int i = 0; i < 4; ++i)
        out[i] = glm::vec4(a.inputs[0][i], 0.0f, 0.0f, 0.0f);
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "combine";
    type.label = "Combine";
    type.category = "Vector";
    type.description = "Builds a value from numbers.";
    type.inputs = {input("R", F),
                   input("G", F),
                   input("B", F),
                   input("A", F, glm::vec4(1.0f))};
    type.outputs = {output("RGBA", V4), output("RGB", V3), output("RG", V2)};
    type.emit = [](const EmitArgs& a)
    {
      const Inputs in = a.inputs;
      return std::vector<std::string>{
          "vec4f(" + in[0] + ", " + in[1] + ", " + in[2] + ", " + in[3] + ")",
          "vec3f(" + in[0] + ", " + in[1] + ", " + in[2] + ")",
          "vec2f(" + in[0] + ", " + in[1] + ")"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      const auto& in = a.inputs;
      out[0] = {in[0].x, in[1].x, in[2].x, in[3].x};
      out[1] = {in[0].x, in[1].x, in[2].x, 0.0f};
      out[2] = {in[0].x, in[1].x, 0.0f, 0.0f};
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "dot";
    type.label = "Dot Product";
    type.category = "Vector";
    type.description = "How much A and B point the same way (for unit vectors, "
                       "1 alike to -1 opposite).";
    type.inputs = {dynamicInput("A", 0.0f), dynamicInput("B", 0.0f)};
    type.outputs = {output("Out", F)};
    type.emit = [](const EmitArgs& a)
    {
      // WGSL's dot takes vectors only.
      if (a.dynamic == F)
        return std::vector<std::string>{"(" + a.inputs[0] + " * " +
                                        a.inputs[1] + ")"};
      return std::vector<std::string>{call("dot", a.inputs)};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(glm::dot(a.inputs[0], a.inputs[1]), 0, 0, 0); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "cross";
    type.label = "Cross Product";
    type.category = "Vector";
    type.description = "A vector at right angles to both A and B.";
    type.inputs = {input("A", V3), input("B", V3)};
    type.outputs = {output("Out", V3)};
    type.emit = [](const EmitArgs& a)
    { return std::vector<std::string>{call("cross", a.inputs)}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      out[0] = glm::vec4(
          glm::cross(glm::vec3(a.inputs[0]), glm::vec3(a.inputs[1])), 0.0f);
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "length";
    type.label = "Length";
    type.category = "Vector";
    type.description = "How long a vector is.";
    type.inputs = {dynamicInput("In", 0.0f)};
    type.outputs = {output("Out", F)};
    type.emit = [](const EmitArgs& a)
    { return std::vector<std::string>{call("length", a.inputs)}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(glm::length(a.inputs[0]), 0, 0, 0); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "distance";
    type.label = "Distance";
    type.category = "Vector";
    type.description = "How far apart A and B are.";
    type.inputs = {dynamicInput("A", 0.0f), dynamicInput("B", 0.0f)};
    type.outputs = {output("Out", F)};
    type.emit = [](const EmitArgs& a)
    { return std::vector<std::string>{call("distance", a.inputs)}; };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = glm::vec4(glm::length(a.inputs[0] - a.inputs[1]), 0, 0, 0); };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "normalize";
    type.label = "Normalize";
    type.category = "Vector";
    type.description = "The vector made 1 long, pointing the same way (a "
                       "number becomes its sign).";
    type.inputs = {dynamicInput("In", 0.0f)};
    type.outputs = {dynamicOutput("Out")};
    type.emit = [](const EmitArgs& a)
    {
      // WGSL's normalize takes vectors only; a number's direction is its
      // sign.
      return std::vector<std::string>{
          call(a.dynamic == F ? "sign" : "normalize", a.inputs)};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      if (a.dynamic == F)
        out[0] = glm::vec4(glm::sign(a.inputs[0].x), 0, 0, 0);
      else
        out[0] = a.inputs[0] / glm::length(a.inputs[0]);
    };
    types.push_back(std::move(type));
  }

  // --- UV
  {
    NodeType type;
    type.id = "tilingOffset";
    type.label = "Tiling and Offset";
    type.category = "UV";
    type.description =
        "Texture coordinates repeated Tiling times and shifted by Offset.";
    type.inputs = {fallbackInput("UV", V2, Fallback::Uv),
                   input("Tiling", V2, glm::vec4(1.0f, 1.0f, 0.0f, 0.0f)),
                   input("Offset", V2)};
    type.outputs = {output("UV", V2)};
    type.emit = [](const EmitArgs& a)
    {
      return std::vector<std::string>{"(" + a.inputs[0] + " * " + a.inputs[1] +
                                      " + " + a.inputs[2] + ")"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    { out[0] = a.inputs[0] * a.inputs[1] + a.inputs[2]; };
    types.push_back(std::move(type));
  }

  // --- Effects
  {
    NodeType type;
    type.id = "fresnel";
    type.label = "Fresnel";
    type.category = "Effects";
    type.description = "Bright where the surface turns away from the camera, "
                       "at its edges: rims and glows. "
                       "Higher powers make thinner rims.";
    type.inputs = {fallbackInput("Normal", V3, Fallback::Normal),
                   fallbackInput("View Direction", V3, Fallback::ViewDirection),
                   input("Power", F, glm::vec4(5.0f))};
    type.outputs = {output("Out", F)};
    type.emit = [](const EmitArgs& a)
    {
      return std::vector<std::string>{
          "pow(1.0 - saturate(dot(normalize(" + a.inputs[0] + "), normalize(" +
          a.inputs[1] + "))), " + a.inputs[2] + ")"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      const float facing =
          glm::clamp(glm::dot(glm::normalize(glm::vec3(a.inputs[0])),
                              glm::normalize(glm::vec3(a.inputs[1]))),
                     0.0f,
                     1.0f);
      out[0] = glm::vec4(std::pow(1.0f - facing, a.inputs[2].x), 0, 0, 0);
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "noise";
    type.label = "Noise";
    type.category = "Patterns";
    type.description = "Smooth random blotches between 0 and 1 (gradient "
                       "noise); Scale sets how many fit "
                       "across the texture coordinates.";
    type.inputs = {fallbackInput("UV", V2, Fallback::Uv),
                   input("Scale", F, glm::vec4(10.0f))};
    type.outputs = {output("Out", F)};
    type.helpers = kNoiseHelpers;
    type.emit = [](const EmitArgs& a)
    {
      return std::vector<std::string>{"noiseGradientNoise(" + a.inputs[0] +
                                      " * " + a.inputs[1] + ")"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      out[0] = glm::vec4(
          gradientNoise(glm::vec2(a.inputs[0]) * a.inputs[1].x), 0, 0, 0);
    };
    types.push_back(std::move(type));
  }

  // --- Patterns (Noise above is one too)
  {
    NodeType type;
    type.id = "voronoi";
    type.label = "Voronoi";
    type.category = "Patterns";
    type.description = "Cells, like cobbles, scales or cracked mud: Distance "
                       "is how far the point is from its "
                       "cell's middle (0 there), Cell a random number (0 to 1) "
                       "for each cell. Randomness 0 "
                       "lines the cells up in a grid.";
    type.inputs = {fallbackInput("UV", V2, Fallback::Uv),
                   input("Scale", F, glm::vec4(5.0f)),
                   unitInput("Randomness", 1.0f)};
    type.outputs = {output("Distance", F), output("Cell", F)};
    type.helpers = kVoronoiHelpers;
    type.emit = [](const EmitArgs& a)
    {
      *a.body += "  let " + a.local + " = voronoiCells(" + a.inputs[0] + " * " +
                 a.inputs[1] + ", " + a.inputs[2] + ");\n";
      return std::vector<std::string>{a.local + ".x", a.local + ".y"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      const glm::vec2 cells =
          voronoiCells(glm::vec2(a.inputs[0]) * a.inputs[1].x, a.inputs[2].x);
      out[0] = glm::vec4(cells.x, 0, 0, 0);
      out[1] = glm::vec4(cells.y, 0, 0, 0);
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "checkerboard";
    type.label = "Checkerboard";
    type.category = "Patterns";
    type.description = "Squares of two colours; Scale sets how many fit across "
                       "the texture coordinates.";
    type.inputs = {fallbackInput("UV", V2, Fallback::Uv),
                   input("Scale", V2, glm::vec4(8.0f, 8.0f, 0.0f, 0.0f)),
                   colorInput("Colour A", glm::vec4(0.15f)),
                   colorInput("Colour B", glm::vec4(0.85f))};
    type.outputs = {output("Colour", V3)};
    type.emit = [](const EmitArgs& a)
    {
      const std::string& l = a.local;
      *a.body += "  let " + l + "c = floor(" + a.inputs[0] + " * " +
                 a.inputs[1] + ");\n";
      *a.body += "  let " + l + "m = " + l + "c.x + " + l +
                 "c.y - 2.0 * floor((" + l + "c.x + " + l + "c.y) * 0.5);\n";
      return std::vector<std::string>{"mix(" + a.inputs[2] + ", " +
                                      a.inputs[3] + ", " + l + "m)"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      const glm::vec2 cell =
          glm::floor(glm::vec2(a.inputs[0]) * glm::vec2(a.inputs[1]));
      const float sum = cell.x + cell.y;
      const float odd = sum - 2.0f * std::floor(sum * 0.5f);
      out[0] = glm::mix(a.inputs[2], a.inputs[3], odd);
    };
    types.push_back(std::move(type));
  }

  // --- Textures that need more than a lookup
  {
    NodeType type;
    type.id = "normalMap";
    type.label = "Normal Map";
    type.category = "Input";
    type.description =
        "Reads a normal map (bumps and grooves painted as colours, OpenGL "
        "style: green up, as "
        "Blender and glTF make them), for the Material Output's Normal. "
        "Strength makes the bumps "
        "deeper (above 1) or shallower; with no image it's flat.";
    type.inputs = {fallbackInput("UV", V2, Fallback::Uv),
                   input("Strength", F, glm::vec4(1.0f))};
    type.outputs = {output("Normal", V3)};
    type.property = Property::Texture;
    type.dataTexture = true;
    type.missingTexture = kFlatNormal;
    type.emit = [](const EmitArgs& a)
    {
      const std::string& l = a.local;
      *a.body += "  let " + l + " = textureSample(" + a.property + ", " +
                 a.sampler + ", " + a.inputs[0] + ").xyz * 2.0 - 1.0;\n";
      return std::vector<std::string>{"normalize(vec3f(" + l + ".xy * " +
                                      a.inputs[1] + ", max(" + l +
                                      ".z, 0.0001)))"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      const glm::vec3 texel =
          a.texture
              ? glm::vec3(sampleTexture(a.texture, glm::vec2(a.inputs[0])))
              : glm::vec3(kFlatNormal);
      const glm::vec3 n = texel * 2.0f - 1.0f;
      out[0] = glm::vec4(glm::normalize(glm::vec3(glm::vec2(n) * a.inputs[1].x,
                                                  std::max(n.z, 0.0001f))),
                         0.0f);
    };
    types.push_back(std::move(type));
  }
  {
    NodeType type;
    type.id = "triplanar";
    type.label = "Triplanar Texture";
    type.category = "Input";
    type.description = "Reads an image projected from the three axes and "
                       "blended by which way the surface "
                       "faces, so it needs no texture coordinates and never "
                       "stretches: terrain, rocks, "
                       "generated meshes. Scale is repeats per metre; "
                       "Sharpness narrows the blends.";
    type.inputs = {fallbackInput("Position", V3, Fallback::Position),
                   fallbackInput("Normal", V3, Fallback::Normal),
                   input("Scale", F, glm::vec4(1.0f)),
                   input("Sharpness", F, glm::vec4(4.0f))};
    type.outputs = {output("RGBA", V4), output("RGB", V3)};
    type.property = Property::Texture;
    type.emit = [](const EmitArgs& a)
    {
      const std::string& l = a.local;
      const auto sample = [&](const char* plane)
      {
        return "textureSample(" + a.property + ", " + a.sampler + ", " + l +
               "p." + plane + ")";
      };
      *a.body += "  let " + l + "b = pow(abs(normalize(" + a.inputs[1] +
                 ")), vec3f(" + a.inputs[3] + "));\n";
      *a.body += "  let " + l + "w = " + l + "b / max(" + l + "b.x + " + l +
                 "b.y + " + l + "b.z, 0.00001);\n";
      *a.body +=
          "  let " + l + "p = " + a.inputs[0] + " * " + a.inputs[2] + ";\n";
      *a.body += "  let " + l + " = " + sample("yz") + " * " + l + "w.x + " +
                 sample("xz") + " * " + l + "w.y + " + sample("xy") + " * " +
                 l + "w.z;\n";
      return std::vector<std::string>{l, l + ".rgb"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      const glm::vec3 blend =
          glm::pow(glm::abs(glm::normalize(glm::vec3(a.inputs[1]))),
                   glm::vec3(a.inputs[3].x));
      const glm::vec3 weights =
          blend / std::max(blend.x + blend.y + blend.z, 0.00001f);
      const glm::vec3 p = glm::vec3(a.inputs[0]) * a.inputs[2].x;
      const glm::vec4 texel = sampleTexture(a.texture, {p.y, p.z}) * weights.x +
                              sampleTexture(a.texture, {p.x, p.z}) * weights.y +
                              sampleTexture(a.texture, {p.x, p.y}) * weights.z;
      out[0] = texel;
      out[1] = glm::vec4(glm::vec3(texel), 0.0f);
    };
    types.push_back(std::move(type));
  }

  {
    NodeType type;
    type.id = "triplanarNormalMap";
    type.reads =
        PointInputs::Tangent | PointInputs::Normal; // into tangent space
    type.label = "Triplanar Normal Map";
    type.category = "Input";
    type.description = "A normal map projected from the three axes like "
                       "Triplanar Texture, for the Material "
                       "Output's Normal: bumps on terrain, rocks and generated "
                       "meshes, with no texture "
                       "coordinates needed. Strength makes them deeper; with "
                       "no image it's flat.";
    type.inputs = {fallbackInput("Position", V3, Fallback::Position),
                   fallbackInput("Normal", V3, Fallback::Normal),
                   input("Scale", F, glm::vec4(1.0f)),
                   input("Sharpness", F, glm::vec4(4.0f)),
                   input("Strength", F, glm::vec4(1.0f))};
    type.outputs = {output("Normal", V3)};
    type.property = Property::Texture;
    type.dataTexture = true;
    type.missingTexture = kFlatNormal;
    type.emit = [](const EmitArgs& a)
    {
      // Each projection's bumps, turned into world directions (the image's
      // +u and up on each plane, as Triplanar Texture lays them), added to
      // the surface's normal by how much it faces that axis; then the result
      // in the point's tangent frame.
      const std::string& l = a.local;
      const auto bumps = [&](const char* plane)
      {
        return "(textureSample(" + a.property + ", " + a.sampler + ", " + l +
               "p." + plane + ").xyz * 2.0 - 1.0)";
      };
      std::string& body = *a.body;
      body += "  let " + l + "n = normalize(" + a.inputs[1] + ");\n";
      body += "  let " + l + "b = pow(abs(" + l + "n), vec3f(" + a.inputs[3] +
              "));\n";
      body += "  let " + l + "w = " + l + "b / max(" + l + "b.x + " + l +
              "b.y + " + l + "b.z, 0.00001) * " + a.inputs[4] + ";\n";
      body += "  let " + l + "p = " + a.inputs[0] + " * " + a.inputs[2] + ";\n";
      body += "  let " + l + "x = " + bumps("yz") + ";\n";
      body += "  let " + l + "y = " + bumps("xz") + ";\n";
      body += "  let " + l + "z = " + bumps("xy") + ";\n";
      body += "  let " + l + " = normalize(" + l + "n + vec3f(0.0, " + l +
              "x.x, -" + l + "x.y) * " + l + "w.x + vec3f(" + l +
              "y.x, 0.0, -" + l + "y.y) * " + l + "w.y + vec3f(" + l +
              "z.x, -" + l + "z.y, 0.0) * " + l + "w.z);\n";
      return std::vector<std::string>{"vec3f(dot(" + l + ", in.tangent), dot(" +
                                      l + ", in.bitangent), dot(" + l +
                                      ", in.normal))"};
    };
    type.evaluate = [](const EvaluateArgs& a, Outputs out)
    {
      const glm::vec3 normal = glm::normalize(glm::vec3(a.inputs[1]));
      const glm::vec3 blend =
          glm::pow(glm::abs(normal), glm::vec3(a.inputs[3].x));
      const glm::vec3 weights =
          blend / std::max(blend.x + blend.y + blend.z, 0.00001f) *
          a.inputs[4].x;
      const glm::vec3 p = glm::vec3(a.inputs[0]) * a.inputs[2].x;
      const auto bumps = [&](glm::vec2 uv)
      {
        return (a.texture ? glm::vec3(sampleTexture(a.texture, uv))
                          : glm::vec3(kFlatNormal)) *
                   2.0f -
               1.0f;
      };
      const glm::vec3 x = bumps({p.y, p.z}), y = bumps({p.x, p.z}),
                      z = bumps({p.x, p.y});
      const glm::vec3 world =
          glm::normalize(normal + glm::vec3(0.0f, x.x, -x.y) * weights.x +
                         glm::vec3(y.x, 0.0f, -y.y) * weights.y +
                         glm::vec3(z.x, -z.y, 0.0f) * weights.z);
      const SurfacePoint& point = *a.point;
      out[0] = glm::vec4(glm::dot(world, point.tangent),
                         glm::dot(world, point.bitangent),
                         glm::dot(world, point.normal),
                         0.0f);
    };
    types.push_back(std::move(type));
  }

  return types;
}

float srgbToLinear(uint8_t value)
{
  const float c = value / 255.0f;
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

const char* typeName(ValueType type)
{
  switch (type)
  {
  case ValueType::Vec2:
    return "Vector 2";
  case ValueType::Vec3:
    return "Vector 3";
  case ValueType::Vec4:
    return "Vector 4";
  case ValueType::Float:
    break;
  }
  return "Number";
}

const char* fallbackName(Fallback fallback)
{
  switch (fallback)
  {
  case Fallback::Uv:
    return "the mesh's UV";
  case Fallback::Normal:
    return "the surface's normal";
  case Fallback::ViewDirection:
    return "the direction to the camera";
  case Fallback::Position:
    return "the point's position";
  case Fallback::Value:
    break;
  }
  return "";
}

const char* wgslType(ValueType type)
{
  switch (type)
  {
  case ValueType::Vec2:
    return "vec2f";
  case ValueType::Vec3:
    return "vec3f";
  case ValueType::Vec4:
    return "vec4f";
  case ValueType::Float:
    break;
  }
  return "f32";
}

std::optional<uint32_t> NodeType::findInput(std::string_view name) const
{
  for (std::size_t i = 0; i < inputs.size(); ++i)
    if (inputs[i].name == name)
      return static_cast<uint32_t>(i);
  return std::nullopt;
}

std::optional<uint32_t> NodeType::findOutput(std::string_view name) const
{
  for (std::size_t i = 0; i < outputs.size(); ++i)
    if (outputs[i].name == name)
      return static_cast<uint32_t>(i);
  return std::nullopt;
}

const std::vector<NodeType>& nodeTypes()
{
  static const std::vector<NodeType> types = makeNodeTypes();
  return types;
}

const NodeType* findNodeType(std::string_view id)
{
  for (const NodeType& type : nodeTypes())
    if (type.id == id)
      return &type;
  return nullptr;
}

std::string convert(const std::string& expression, ValueType from, ValueType to)
{
  if (from == to)
    return expression;
  if (from == ValueType::Float)
    return std::string(wgslType(to)) + "(" + expression + ")";
  if (to == ValueType::Float)
    return "(" + expression + ").x";
  if (width(to) < width(from))
    return "(" + expression + ")." + std::string("xyzw").substr(0, width(to));
  // Wider: zeros, and an alpha of 1 for a Vector 4.
  std::string padding;
  for (int i = width(from); i < width(to); ++i)
    padding += (i == 3) ? ", 1.0" : ", 0.0";
  return std::string(wgslType(to)) + "(" + expression + padding + ")";
}

glm::vec4 convert(glm::vec4 value, ValueType from, ValueType to)
{
  if (from == ValueType::Float)
    return trim(glm::vec4(value.x), to);
  glm::vec4 out = trim(value, std::min(from, to));
  if (width(from) < 4 && to == ValueType::Vec4)
    out.w = 1.0f;
  return out;
}

glm::vec4 trim(glm::vec4 value, ValueType type)
{
  for (int i = width(type); i < 4; ++i)
    value[i] = 0.0f;
  return value;
}

void textureDirections(const glm::vec3 positions[3],
                       const glm::vec2 uvs[3],
                       glm::vec3& tangent,
                       glm::vec3& bitangent)
{
  const glm::vec3 e1 = positions[1] - positions[0],
                  e2 = positions[2] - positions[0];
  const glm::vec2 d1 = uvs[1] - uvs[0], d2 = uvs[2] - uvs[0];
  const float det = d1.x * d2.y - d2.x * d1.y;
  if (std::abs(det) < 1e-12f)
  {
    tangent = bitangent = glm::vec3(0.0f);
    return;
  }
  // Solve e = dp/du * du + dp/dv * dv for both edges.
  tangent = (e1 * d2.y - e2 * d1.y) / det;
  bitangent = -(e2 * d1.x - e1 * d2.x) / det;
}

void tangentFrame(const glm::vec3& normal,
                  const glm::vec3& alongU,
                  const glm::vec3& upImage,
                  glm::vec3& tangent,
                  glm::vec3& bitangent)
{
  const glm::vec3 flatU = alongU - normal * glm::dot(normal, alongU);
  const glm::vec3 flatUp = upImage - normal * glm::dot(normal, upImage);
  if (glm::dot(flatU, flatU) > 1e-20f)
  {
    tangent = glm::normalize(flatU);
    bitangent = glm::cross(normal, tangent);
    if (glm::dot(bitangent, flatUp) < 0.0f) // a mirrored texture
      bitangent = -bitangent;
  }
  else if (glm::dot(flatUp, flatUp) > 1e-20f)
  {
    bitangent = glm::normalize(flatUp);
    tangent = glm::cross(bitangent, normal);
  }
  else
  {
    const glm::vec3 reference =
        std::abs(normal.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    tangent = glm::normalize(glm::cross(reference, normal));
    bitangent = glm::cross(normal, tangent);
  }
}

glm::vec3 perturbNormal(const SurfacePoint& point,
                        const glm::vec3& tangentNormal)
{
  return glm::normalize(point.tangent * tangentNormal.x +
                        point.bitangent * tangentNormal.y +
                        point.normal * tangentNormal.z);
}

glm::vec4 sampleTexture(const TextureData* texture, glm::vec2 uv)
{
  if (!texture || texture->empty())
    return glm::vec4(1.0f);

  const bool linear = texture->linear;
  const auto texel = [&](int x, int y)
  {
    // Repeat: wrap into the image.
    const int w = static_cast<int>(texture->width),
              h = static_cast<int>(texture->height);
    x = ((x % w) + w) % w;
    y = ((y % h) + h) % h;
    const uint8_t* p = &texture->rgba[(std::size_t(y) * w + x) * 4];
    if (linear)
      return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
    return glm::vec4(srgbToLinear(p[0]),
                     srgbToLinear(p[1]),
                     srgbToLinear(p[2]),
                     p[3] / 255.0f);
  };

  // Texel centres sit at half-texel positions, as on the GPU.
  const glm::vec2 position =
      uv * glm::vec2(texture->width, texture->height) - 0.5f;
  if (texture->nearest)
  {
    const glm::vec2 nearest = glm::floor(position + 0.5f);
    return texel(static_cast<int>(nearest.x), static_cast<int>(nearest.y));
  }
  const glm::vec2 base = glm::floor(position);
  const glm::vec2 t = position - base;
  const int x = static_cast<int>(base.x), y = static_cast<int>(base.y);
  return glm::mix(glm::mix(texel(x, y), texel(x + 1, y), t.x),
                  glm::mix(texel(x, y + 1), texel(x + 1, y + 1), t.x),
                  t.y);
}

} // namespace sfs::materialGraph
