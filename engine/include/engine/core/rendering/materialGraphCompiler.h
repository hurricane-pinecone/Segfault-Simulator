#pragma once

#include "engine/core/rendering/materialGraph.h"
#include "engine/core/rendering/materialNodes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{
struct TextureData;
}

namespace sfs::materialGraph
{

/** A surface, as a graph's Material Output gives it for one point. */
struct SurfaceValues
{
  glm::vec3 baseColor{1.0f}; // linear RGB
  float alpha = 1.0f;
  float metallic = 0.0f;
  float roughness = 1.0f;
  // Which way the surface faces, in tangent space (a normal map's): (0, 0, 1)
  // is the mesh's own normal.
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  glm::vec3 emission{0.0f}; // light the surface gives off, added after lighting
  float alphaClip = 0.5f;   // cut-out materials drop points below this alpha
  float bloom = 1.0f;       // how much of its light blooms
};

/**
 * What a graph compiles to, apart from its values: a WGSL surface function,
 * and the same steps for the CPU.
 *
 * The WGSL declares the material's bindings (group 1: binding 0 the
 * parameters, an array of `parameterCount()` vec4f, at least one; then, for
 * each texture, a texture_2d<f32> and its sampler) and
 *
 *   fn surface(in: SurfaceIn) -> Surface
 *
 * written against the renderer's SurfaceIn (uv, position, normal, toView,
 * vertexColor, time) and Surface (baseColor, alpha, metallic, roughness,
 * normal, emission, alphaClip). Graphs that differ only in their values compile
 * to the same WGSL, so a renderer can share one pipeline between them.
 */
class MaterialShader
{
public:
  const std::string& wgsl() const { return m_wgsl; }

  /** How many vec4 parameters, and textures, a material of it supplies. */
  std::size_t parameterCount() const { return m_parameterCount; }
  std::size_t textureCount() const { return m_textureCount; }

  /**
   * What texture `index` reads while a material has no image for it: white,
   * or (for a normal map) a flat normal. A renderer binds a 1x1 texture of
   * it.
   */
  glm::vec4 missingTexture(std::size_t index) const
  {
    return index < m_missingTextures.size() ? m_missingTextures[index]
                                            : glm::vec4(1.0f);
  }

  /**
   * What the Material Output's input `output` (kBaseColor..kBloom)
   * depends on of the point, through every node that feeds it. None: the
   * same everywhere (for given parameters and textures), so evaluating it at
   * any one point gives it.
   */
  PointInputs reads(uint32_t output) const
  {
    return output < m_outputReads.size() ? m_outputReads[output]
                                         : PointInputs::None;
  }

  /**
   * The surface at a point, worked out on the CPU (for software previews).
   * Missing parameters read 0 and missing textures white.
   */
  SurfaceValues
  evaluate(const SurfacePoint& point,
           std::span<const glm::vec4> parameters,
           std::span<const std::shared_ptr<const TextureData>> textures) const;

private:
  friend struct Compiler;

  // Where a node's input value comes from.
  struct Source
  {
    enum class Kind : uint8_t
    {
      Register,  // an earlier node's output
      Parameter, // a parameter (an input's or the output's own value)
      Uv,
      Normal,
      ToView,
      Position,
    };
    Kind kind = Kind::Parameter;
    uint32_t index = 0; // register or parameter
    ValueType from = ValueType::Float;
    ValueType to = ValueType::Float;
  };

  struct Step
  {
    const NodeType* type = nullptr;
    std::vector<Source> inputs;
    uint32_t firstOutput = 0; // its outputs' registers start here
    ValueType dynamic = ValueType::Float;
    int32_t parameter = -1; // a Colour or Number node's value
    int32_t texture = -1;
  };

  // Fill m_outputReads, once the steps and outputs are set.
  void findOutputReads();

  glm::vec4 read(const Source& source,
                 std::span<const glm::vec4> registers,
                 std::span<const glm::vec4> parameters,
                 const SurfacePoint& point) const;

  std::string m_wgsl;
  std::vector<Step> m_steps;
  std::array<Source, kOutputInputCount>
      m_output{}; // the Material Output's inputs
  std::array<PointInputs, kOutputInputCount> m_outputReads{}; // what each reads
  uint32_t m_registerCount = 0;
  std::size_t m_parameterCount = 0;
  std::size_t m_textureCount = 0;
  std::vector<glm::vec4> m_missingTextures; // per texture
};

/** A texture a compiled graph reads: the Texture node's image. */
struct TextureSlot
{
  NodeId node = 0;
  std::string texture; // "asset:<id>"; empty: white
  bool data = false;   // read as data, not colour
};

/** Something wrong with a graph, on a node (or, with node 0, the whole graph).
 */
struct CompileError
{
  NodeId node = 0;
  std::string message;
};

struct CompiledGraph
{
  // Null if the graph has errors.
  std::shared_ptr<const MaterialShader> shader;
  // The values and textures a material of this graph supplies, in the
  // shader's order.
  std::vector<glm::vec4> parameters;
  std::vector<TextureSlot> textures;
  std::vector<CompileError> errors;
  // Each node's dynamic type (for every node of a known type, connected to
  // the output or not), so an editor can show what its pins carry.
  std::unordered_map<NodeId, ValueType> dynamicTypes;
};

/**
 * Compile a graph: the nodes feeding its Material Output, in order, into a
 * shader (nodes that feed nothing are left out). Errors: no Material Output,
 * more than one, a node of an unknown type feeding it, or a loop.
 */
CompiledGraph compile(const Graph& graph);

} // namespace sfs::materialGraph
