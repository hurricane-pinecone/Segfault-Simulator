#include "engine/core/rendering/materialGraphCompiler.h"

#include "engine/core/rendering/mesh.h"

#include <algorithm>
#include <unordered_set>

namespace sfs::materialGraph
{

namespace
{

// The parameter's WGSL, narrowed to a type ("graphParameters.values[3].xyz").
std::string parameterWgsl(std::size_t index, ValueType type)
{
  std::string out = "graphParameters.values[" + std::to_string(index) + "]";
  if (type != ValueType::Vec4)
    out += "." + std::string("xyzw").substr(0, width(type));
  return out;
}

std::string outputName(NodeId node, std::size_t output)
{
  return "n" + std::to_string(node) + "_" + std::to_string(output);
}

} // namespace

// Builds a CompiledGraph; a friend of MaterialShader.
struct Compiler
{
  const Graph& graph;
  CompiledGraph result;
  std::shared_ptr<MaterialShader> shader = std::make_shared<MaterialShader>();

  // Every node of a known type, sources before what they feed.
  std::vector<const Node*> order;
  // Each node output's type.
  std::unordered_map<NodeId, std::vector<ValueType>> outputTypes;
  // Each node output's register (the CPU program) once emitted.
  std::unordered_map<NodeId, uint32_t> firstRegister;

  void error(NodeId node, std::string message)
  {
    result.errors.push_back({node, std::move(message)});
  }

  // Sort the nodes so each comes after everything feeding it; nodes in a
  // loop are left out (and reported).
  void sort()
  {
    std::unordered_map<NodeId, int>
        waiting; // links into each node not yet placed
    for (const Node& node : graph.nodes)
      waiting[node.id] = 0;
    for (const Link& link : graph.links)
      if (waiting.contains(link.fromNode) && waiting.contains(link.toNode))
        ++waiting[link.toNode];

    std::vector<const Node*> ready;
    for (const Node& node : graph.nodes)
      if (waiting[node.id] == 0)
        ready.push_back(&node);
    // Stable: ties go in the graph's order, so the WGSL doesn't change unless
    // the graph does.
    std::reverse(ready.begin(), ready.end());
    std::vector<const Node*> sorted;
    while (!ready.empty())
    {
      const Node* node = ready.back();
      ready.pop_back();
      sorted.push_back(node);
      std::vector<const Node*> next;
      for (const Link& link : graph.links)
        if (link.fromNode == node->id && waiting.contains(link.toNode) &&
            --waiting[link.toNode] == 0)
          next.push_back(graph.find(link.toNode));
      ready.insert(ready.end(), next.rbegin(), next.rend());
    }
    for (const Node& node : graph.nodes)
      if (waiting[node.id] > 0)
        error(node.id, "This node feeds into itself");
    for (const Node* node : sorted)
      if (findNodeType(node->type))
        order.push_back(node);
  }

  // The source of a node's input: its link's node and output, if it has a
  // valid one.
  const Link* source(const Node& node, uint32_t input) const
  {
    const Link* link = graph.linkInto(node.id, input);
    if (!link)
      return nullptr;
    const auto it = outputTypes.find(link->fromNode);
    if (it == outputTypes.end() || link->fromOutput >= it->second.size())
      return nullptr;
    return link;
  }

  void resolveTypes()
  {
    for (const Node* node : order)
    {
      const NodeType& type = *findNodeType(node->type);
      ValueType dynamic = ValueType::Float;
      for (uint32_t i = 0; i < type.inputs.size(); ++i)
        if (!type.inputs[i].type)
          if (const Link* link = source(*node, i))
            dynamic = std::max(
                dynamic, outputTypes[link->fromNode][link->fromOutput]);
      result.dynamicTypes[node->id] = dynamic;
      std::vector<ValueType>& outputs = outputTypes[node->id];
      for (std::size_t i = 0; i < type.outputs.size(); ++i)
        outputs.push_back(type.outputType(i, dynamic));
    }
  }

  // The nodes that feed the output (and the output itself).
  std::unordered_set<NodeId> feedingOutput(NodeId output) const
  {
    std::unordered_set<NodeId> found = {output};
    std::vector<NodeId> pending = {output};
    while (!pending.empty())
    {
      const NodeId node = pending.back();
      pending.pop_back();
      for (const Link& link : graph.links)
        if (link.toNode == node && found.insert(link.fromNode).second)
          pending.push_back(link.fromNode);
    }
    return found;
  }

  std::size_t addParameter(const glm::vec4& value)
  {
    result.parameters.push_back(value);
    return result.parameters.size() - 1;
  }

  // An input's WGSL and CPU source.
  std::pair<std::string, MaterialShader::Source>
  input(const Node& node, const NodeType& type, uint32_t index)
  {
    using Kind = MaterialShader::Source::Kind;
    const ValueType to = type.inputType(index, result.dynamicTypes[node.id]);
    if (const Link* link = source(node, index))
    {
      const ValueType from = outputTypes[link->fromNode][link->fromOutput];
      return {convert(outputName(link->fromNode, link->fromOutput), from, to),
              {Kind::Register,
               firstRegister.at(link->fromNode) + link->fromOutput,
               from,
               to}};
    }
    switch (type.inputs[index].fallback)
    {
    case Fallback::Uv:
      return {convert("in.uv", ValueType::Vec2, to),
              {Kind::Uv, 0, ValueType::Vec2, to}};
    case Fallback::Normal:
      return {convert("in.normal", ValueType::Vec3, to),
              {Kind::Normal, 0, ValueType::Vec3, to}};
    case Fallback::ViewDirection:
      return {convert("in.toView", ValueType::Vec3, to),
              {Kind::ToView, 0, ValueType::Vec3, to}};
    case Fallback::Position:
      return {convert("in.position", ValueType::Vec3, to),
              {Kind::Position, 0, ValueType::Vec3, to}};
    case Fallback::Value:
      break;
    }
    const glm::vec4 value = index < node.inputs.size()
                                ? node.inputs[index]
                                : type.inputs[index].defaultValue;
    const std::size_t parameter = addParameter(trim(value, to));
    return {parameterWgsl(parameter, to),
            {Kind::Parameter, static_cast<uint32_t>(parameter), to, to}};
  }

  void run()
  {
    sort();
    resolveTypes();

    std::vector<const Node*> outputs;
    for (const Node& node : graph.nodes)
      if (node.type == kOutputType)
        outputs.push_back(&node);
    if (outputs.empty())
      error(0, "The graph needs a Material Output");
    for (std::size_t i = 1; i < outputs.size(); ++i)
      error(outputs[i]->id, "A graph has one Material Output: remove this one");
    if (outputs.empty())
      return;
    const Node& output = *outputs.front();

    const std::unordered_set<NodeId> used = feedingOutput(output.id);
    for (const Node& node : graph.nodes)
      if (used.contains(node.id) && !findNodeType(node.type))
        error(node.id,
              "Unknown node type \"" + node.type +
                  "\" (made by a newer version?)");
    if (!result.errors.empty())
      return;

    std::string helpers;
    std::unordered_set<const NodeType*> withHelpers;
    std::string body;
    for (const Node* node : order)
    {
      if (!used.contains(node->id) || node->id == output.id)
        continue;
      const NodeType& type = *findNodeType(node->type);
      if (!type.helpers.empty() && withHelpers.insert(&type).second)
        helpers += type.helpers;

      MaterialShader::Step step;
      step.type = &type;
      step.dynamic = result.dynamicTypes[node->id];
      std::vector<std::string> inputs;
      for (uint32_t i = 0; i < type.inputs.size(); ++i)
      {
        auto [wgsl, source] = input(*node, type, i);
        inputs.push_back(std::move(wgsl));
        step.inputs.push_back(source);
      }

      EmitArgs args;
      args.inputs = inputs;
      args.dynamic = step.dynamic;
      args.local = "n" + std::to_string(node->id);
      args.body = &body;
      if (type.property == Property::Color || type.property == Property::Number)
      {
        const std::size_t parameter = addParameter(node->value);
        step.parameter = static_cast<int32_t>(parameter);
        args.property = parameterWgsl(parameter, ValueType::Vec4);
      }
      if (type.property == Property::Texture)
      {
        const std::size_t texture = result.textures.size();
        const bool data = node->dataTexture || type.dataTexture;
        result.textures.push_back({node->id, node->texture, data});
        step.texture = static_cast<int32_t>(texture);
        shader->m_missingTextures.push_back(type.missingTexture);
        args.property = "graphTexture" + std::to_string(texture);
        args.sampler = "graphSampler" + std::to_string(texture);
      }

      const std::vector<std::string> expressions = type.emit(args);
      const std::vector<ValueType>& types = outputTypes[node->id];
      for (std::size_t i = 0; i < expressions.size() && i < types.size(); ++i)
        body += "  let " + outputName(node->id, i) + ": " + wgslType(types[i]) +
                " = " + expressions[i] + ";\n";

      step.firstOutput = shader->m_registerCount;
      firstRegister[node->id] = shader->m_registerCount;
      shader->m_registerCount += static_cast<uint32_t>(type.outputs.size());
      shader->m_steps.push_back(std::move(step));
    }

    // The Material Output's inputs become the surface.
    const NodeType& outputType = *findNodeType(kOutputType);
    static constexpr const char* kFields[] = {"baseColor",
                                              "metallic",
                                              "roughness",
                                              "normal",
                                              "emission",
                                              "alphaClip",
                                              "alpha",
                                              "bloom"};
    static constexpr uint32_t kFieldInputs[] = {kBaseColor,
                                                kMetallic,
                                                kRoughness,
                                                kNormal,
                                                kEmission,
                                                kAlphaClip,
                                                kAlpha,
                                                kBloom};
    body += "  var result: Surface;\n";
    for (std::size_t i = 0; i < std::size(kFields); ++i)
    {
      auto [wgsl, source] = input(output, outputType, kFieldInputs[i]);
      body += "  result." + std::string(kFields[i]) + " = " + wgsl + ";\n";
      shader->m_output[kFieldInputs[i]] = source;
    }
    body += "  return result;\n";

    std::string wgsl =
        "struct GraphParameters {\n  values: array<vec4f, " +
        std::to_string(std::max<std::size_t>(result.parameters.size(), 1)) +
        ">,\n}\n\n@group(1) @binding(0) var<uniform> graphParameters: "
        "GraphParameters;\n";
    for (std::size_t i = 0; i < result.textures.size(); ++i)
    {
      const std::string n = std::to_string(i);
      wgsl += "@group(1) @binding(" + std::to_string(1 + 2 * i) +
              ") var graphTexture" + n + ": texture_2d<f32>;\n";
      wgsl += "@group(1) @binding(" + std::to_string(2 + 2 * i) +
              ") var graphSampler" + n + ": sampler;\n";
    }
    wgsl += helpers;
    wgsl += "\nfn surface(in: SurfaceIn) -> Surface {\n" + body + "}\n";

    shader->findOutputReads();
    shader->m_wgsl = std::move(wgsl);
    shader->m_parameterCount = result.parameters.size();
    shader->m_textureCount = result.textures.size();
    result.shader = std::move(shader);
  }
};

void MaterialShader::findOutputReads()
{
  // What each register depends on, step by step (a step only reads earlier
  // steps' registers): what its node reads itself, and what its inputs do.
  std::vector<PointInputs> registers(m_registerCount, PointInputs::None);
  const auto readsOf = [&](const Source& source)
  {
    switch (source.kind)
    {
    case Source::Kind::Register:
      return source.index < registers.size() ? registers[source.index]
                                             : PointInputs::None;
    case Source::Kind::Parameter:
      return PointInputs::None;
    case Source::Kind::Uv:
      return PointInputs::Uv;
    case Source::Kind::Normal:
      return PointInputs::Normal;
    case Source::Kind::ToView:
      return PointInputs::ToView;
    case Source::Kind::Position:
      return PointInputs::Position;
    }
    return PointInputs::None;
  };
  for (const Step& step : m_steps)
  {
    PointInputs reads = step.type->reads;
    for (const Source& input : step.inputs)
      reads |= readsOf(input);
    for (std::size_t i = 0; i < step.type->outputs.size(); ++i)
      registers[step.firstOutput + i] = reads;
  }
  for (std::size_t i = 0; i < m_output.size(); ++i)
    m_outputReads[i] = readsOf(m_output[i]);
}

glm::vec4 MaterialShader::read(const Source& source,
                               std::span<const glm::vec4> registers,
                               std::span<const glm::vec4> parameters,
                               const SurfacePoint& point) const
{
  glm::vec4 value(0.0f);
  switch (source.kind)
  {
  case Source::Kind::Register:
    value = registers[source.index];
    break;
  case Source::Kind::Parameter:
    if (source.index < parameters.size())
      value = trim(parameters[source.index], source.from);
    break;
  case Source::Kind::Uv:
    value = glm::vec4(point.uv, 0.0f, 0.0f);
    break;
  case Source::Kind::Normal:
    value = glm::vec4(point.normal, 0.0f);
    break;
  case Source::Kind::ToView:
    value = glm::vec4(point.toView, 0.0f);
    break;
  case Source::Kind::Position:
    value = glm::vec4(point.position, 0.0f);
    break;
  }
  return convert(value, source.from, source.to);
}

SurfaceValues MaterialShader::evaluate(
    const SurfacePoint& point,
    std::span<const glm::vec4> parameters,
    std::span<const std::shared_ptr<const TextureData>> textures) const
{
  // Kept between calls (previews evaluate every pixel), one set per thread.
  thread_local std::vector<glm::vec4> registers;
  thread_local std::vector<glm::vec4> inputs;
  registers.assign(m_registerCount, glm::vec4(0.0f));
  for (const Step& step : m_steps)
  {
    inputs.clear();
    for (const Source& source : step.inputs)
      inputs.push_back(read(source, registers, parameters, point));

    EvaluateArgs args;
    args.inputs = inputs;
    args.dynamic = step.dynamic;
    if (step.parameter >= 0 &&
        static_cast<std::size_t>(step.parameter) < parameters.size())
      args.property = parameters[step.parameter];
    if (step.texture >= 0 &&
        static_cast<std::size_t>(step.texture) < textures.size())
      args.texture = textures[step.texture].get();
    args.point = &point;

    const std::span<glm::vec4> outputs(
        registers.data() + step.firstOutput, step.type->outputs.size());
    step.type->evaluate(args, outputs);
    for (std::size_t i = 0; i < outputs.size(); ++i)
      outputs[i] = trim(outputs[i], step.type->outputType(i, step.dynamic));
  }

  const auto value = [&](uint32_t input)
  { return read(m_output[input], registers, parameters, point); };
  SurfaceValues surface;
  surface.baseColor = glm::vec3(value(kBaseColor));
  surface.alpha = value(kAlpha).x;
  surface.metallic = value(kMetallic).x;
  surface.roughness = value(kRoughness).x;
  surface.normal = glm::vec3(value(kNormal));
  surface.emission = glm::vec3(value(kEmission));
  surface.alphaClip = value(kAlphaClip).x;
  surface.bloom = value(kBloom).x;
  return surface;
}

CompiledGraph compile(const Graph& graph)
{
  Compiler compiler{graph};
  compiler.run();
  return std::move(compiler.result);
}

} // namespace sfs::materialGraph
