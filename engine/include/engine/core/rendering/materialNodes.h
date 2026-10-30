#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{
struct TextureData;
}

namespace sfs::materialGraph
{

/** A value's shape: its number of components, 1 (a number) to 4. */
enum class ValueType : uint8_t
{
  Float = 1,
  Vec2 = 2,
  Vec3 = 3,
  Vec4 = 4,
};

/** Its number of components. */
inline int width(ValueType type) { return static_cast<int>(type); }

/** "Number", "Vector 2", "Vector 3" or "Vector 4", for people. */
const char* typeName(ValueType type);

/** The WGSL type: "f32", "vec2f", "vec3f" or "vec4f". */
const char* wgslType(ValueType type);

/** What an input reads while nothing is connected to it. */
enum class Fallback : uint8_t
{
  Value,         // its own value (Node::inputs), edited on the node
  Uv,            // the mesh's texture coordinates
  Normal,        // the surface's world-space normal
  ViewDirection, // world-space direction from the surface to the camera
  Position,      // the point's world-space position
};

/** What an input reads unlinked, for people ("the mesh's UV"); "" for Value. */
const char* fallbackName(Fallback fallback);

struct InputPin
{
  std::string name;
  // Its type; nothing for a "dynamic" input, which takes the node's dynamic
  // type (the widest of what's connected to its dynamic inputs; a number if
  // nothing is).
  std::optional<ValueType> type;
  glm::vec4 defaultValue{0.0f};
  Fallback fallback = Fallback::Value;
  bool color = false; // its value is edited as a colour
  bool unit = false;  // its value makes sense from 0 to 1 (a slider)
};

struct OutputPin
{
  std::string name;
  std::optional<ValueType> type; // nothing: the node's dynamic type
};

/** What a node edits on itself, besides its inputs' values. */
enum class Property : uint8_t
{
  None,
  Color,   // Node::value, as a colour with alpha
  Number,  // Node::value.x
  Texture, // Node::texture and Node::dataTexture
};

/** Where a point being shaded is, for the nodes that read it. */
struct SurfacePoint
{
  glm::vec2 uv{0.0f};
  glm::vec3 position{0.0f}; // world space
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  // The point's tangent frame (see tangentFrame()): unit, at right angles to
  // each other and to the normal. Tangent space's +x and +y.
  glm::vec3 tangent{1.0f, 0.0f, 0.0f};
  glm::vec3 bitangent{0.0f, 1.0f, 0.0f};
  glm::vec3 toView{0.0f, 0.0f, 1.0f};
  glm::vec3 vertexColor{1.0f};
  float time = 0.0f; // seconds
  // Grass's blade here (see the Grass node): its length (of the grass's full
  // height), how far up it this is (0 root, 1 tip) and a random number; and
  // its maps' four Data channels. Anything else: full length, at the root.
  glm::vec4 grass{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec4 grassData{0.0f};
};

/**
 * Parts of a SurfacePoint, as flags: what a node, or a compiled graph's
 * output, depends on. Something that reads none of them is the same all over
 * a surface, and something that reads none of the placed ones (Position,
 * Normal, Tangent, ToView) is the same wherever and however the object is
 * placed.
 */
enum class PointInputs : uint8_t
{
  None = 0,
  Uv = 1 << 0,
  Position = 1 << 1,
  Normal = 1 << 2,
  Tangent = 1 << 3, // the tangent frame
  ToView = 1 << 4,
  VertexColor = 1 << 5,
  Time = 1 << 6,
  Grass = 1 << 7, // grass and grassData
};

constexpr PointInputs operator|(PointInputs a, PointInputs b)
{
  return static_cast<PointInputs>(static_cast<uint8_t>(a) |
                                  static_cast<uint8_t>(b));
}
constexpr PointInputs operator&(PointInputs a, PointInputs b)
{
  return static_cast<PointInputs>(static_cast<uint8_t>(a) &
                                  static_cast<uint8_t>(b));
}
constexpr PointInputs& operator|=(PointInputs& a, PointInputs b)
{
  return a = a | b;
}
constexpr bool any(PointInputs inputs) { return inputs != PointInputs::None; }

/** The inputs that depend on where and how an object is placed, and seen. */
inline constexpr PointInputs kPlacedInputs =
    PointInputs::Position | PointInputs::Normal | PointInputs::Tangent |
    PointInputs::ToView;

/** What a node's WGSL is written from. */
struct EmitArgs
{
  // Each input's WGSL expression, already of the input's type.
  std::span<const std::string> inputs;
  ValueType dynamic = ValueType::Float; // the node's dynamic type
  std::string property; // the Colour/Number value's or Texture's WGSL name
  std::string sampler;  // a Texture node's sampler
  std::string local;    // a name the node may declare (`let <local> = ...`)
  std::string* body;    // statements to add before the node's outputs
};

/** What a node is worked out from on the CPU (for software previews). */
struct EvaluateArgs
{
  // Each input's value, already of the input's type (components past its
  // width are 0).
  std::span<const glm::vec4> inputs;
  ValueType dynamic = ValueType::Float;
  glm::vec4 property{0.0f};
  const TextureData* texture = nullptr; // null: what its type reads without one
  const SurfacePoint* point = nullptr;
};

/**
 * A kind of node: its pins, and how its outputs are worked out, as WGSL for the
 * GPU and in C++ for previews drawn on the CPU (the two must agree).
 */
struct NodeType
{
  std::string id;    // as saved in files; never changes once released
  std::string label; // for people
  std::string category;
  std::string description;
  std::vector<InputPin> inputs;
  std::vector<OutputPin> outputs;
  Property property = Property::None;
  glm::vec4 propertyDefault{0.0f}; // a new node's Node::value
  // Its texture always holds data (a normal map), whatever Node::dataTexture
  // says.
  bool dataTexture = false;
  // What its texture reads with no image set (white: colour x 1).
  glm::vec4 missingTexture{1.0f};
  // The parts of the point its code reads itself (not through its inputs'
  // fallbacks, which the compiler tracks).
  PointInputs reads = PointInputs::None;
  // WGSL functions the node's code calls, added to the shader once however
  // many nodes use them. Named after the node, so they can't clash.
  std::string helpers;

  // Each output's WGSL expression, of the output's type.
  std::vector<std::string> (*emit)(const EmitArgs&) = nullptr;
  // Each output's value, of the output's type.
  void (*evaluate)(const EvaluateArgs&, std::span<glm::vec4> outputs) = nullptr;

  /** An input's or output's type, with the node's dynamic type filled in. */
  ValueType inputType(std::size_t input, ValueType dynamic) const
  {
    return inputs[input].type.value_or(dynamic);
  }
  ValueType outputType(std::size_t output, ValueType dynamic) const
  {
    return outputs[output].type.value_or(dynamic);
  }

  /** The index of the input or output with this name, if there is one. */
  std::optional<uint32_t> findInput(std::string_view name) const;
  std::optional<uint32_t> findOutput(std::string_view name) const;
};

/** A normal map's texel for a flat surface: (0, 0, 1) packed into 0..1. */
inline constexpr glm::vec4 kFlatNormal{0.5f, 0.5f, 1.0f, 1.0f};

/** The Material Output node's type id. */
inline constexpr std::string_view kOutputType = "output";

/** The Material Output node's inputs, in order. */
enum OutputInput : uint32_t
{
  kBaseColor,
  kAlpha,
  kMetallic,
  kRoughness,
  kNormal, // tangent space: (0, 0, 1) is the mesh's own
  kEmission,
  kAlphaClip,
  kBloom, // how much of its light blooms (x its brightness), 1 normally
};

/** How many inputs the Material Output has. */
inline constexpr uint32_t kOutputInputCount = kBloom + 1;

/** Every node type, the Material Output first, then by category. */
const std::vector<NodeType>& nodeTypes();

/** A node type by id; null for an unknown id. */
const NodeType* findNodeType(std::string_view id);

/**
 * Convert a WGSL expression of type `from` to type `to`: a number fills every
 * component; a wider value drops its last components; a narrower one gains
 * zeros (and an alpha of 1, when it becomes a Vector 4).
 */
std::string
convert(const std::string& expression, ValueType from, ValueType to);

/** The same conversion for a value worked out on the CPU. */
glm::vec4 convert(glm::vec4 value, ValueType from, ValueType to);

/** A value with the components past `type`'s width zeroed. */
glm::vec4 trim(glm::vec4 value, ValueType type);

/**
 * A triangle's texture directions (for SurfacePoint::tangent and bitangent):
 * how its position changes along +u and up the image (-v). Zero if its
 * texture coordinates don't span an area.
 */
void textureDirections(const glm::vec3 positions[3],
                       const glm::vec2 uvs[3],
                       glm::vec3& tangent,
                       glm::vec3& bitangent);

/**
 * A point's tangent frame: unit tangent and bitangent at right angles to each
 * other and to the (unit) normal, so tangent space (normal maps, a graph's
 * Normal) has +x running along the texture (`alongU`, +u) and +y up it
 * (`upImage`, -v), mirrored if the texture is. Where the texture coordinates
 * don't run anywhere (no UVs), a frame from the world's axes: on ground,
 * tangent +x and bitangent +y; on walls, bitangent up (+Z). The GPU's
 * meshSurface.wgsl builds the same.
 */
void tangentFrame(const glm::vec3& normal,
                  const glm::vec3& alongU,
                  const glm::vec3& upImage,
                  glm::vec3& tangent,
                  glm::vec3& bitangent);

/**
 * A surface's tangent-space normal (SurfaceValues::normal) in world space at a
 * point, along its tangent frame.
 */
glm::vec3 perturbNormal(const SurfacePoint& point,
                        const glm::vec3& tangentNormal);

/**
 * Sample a texture at `uv` (repeating, bilinear unless the texture is
 * `nearest`), as linear RGB and alpha, decoded as the GPU does: from sRGB,
 * unless the texture is marked linear (as the asset store makes the images a
 * graph reads as data). A null texture reads white.
 */
glm::vec4 sampleTexture(const TextureData* texture, glm::vec2 uv);

} // namespace sfs::materialGraph
