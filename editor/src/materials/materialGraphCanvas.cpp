#include "materials/materialGraphCanvas.h"

#include "assets/assetDrop.h"
#include "engine/core/assets/assetDatabase.h"
#include "engine/core/rendering/materialGraph.h"
#include "engine/core/rendering/materialGraphCompiler.h"
#include "engine/core/rendering/materialNodes.h"
#include "widgets/boardCanvas.h"
#include "widgets/theme.h"
#include "widgets/widgets.h"

#include <glm/glm/common.hpp>
#include <glm/glm/ext/vector_float4.hpp>
#include <imgui.h>
#include <imgui_internal.h> // ImRect, ImLengthSqr

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mg = sfs::materialGraph;

using board::operator+;
using board::operator-;
using board::operator*;

namespace
{

// Sizes in canvas units (screen pixels at zoom 1). A node's header, rows and
// padding are the board's box style's.
constexpr float PIN_RADIUS = 5.0f;
constexpr float FIELDS_ZOOM = 0.6f; // below this, values show as text

constexpr float SWATCH_WIDTH = 20.0f;

constexpr const char* CLIPBOARD_KEY = "sfsMaterialNodes";

constexpr ImU32 ERROR_COLOR = IM_COL32(230, 96, 86, 255);

// Room for an input's value field (canvas units), by its type.
float fieldWidth(mg::ValueType type)
{
  switch (type)
  {
  case mg::ValueType::Vec2:
    return 104.0f;
  case mg::ValueType::Vec3:
    return 150.0f;
  case mg::ValueType::Vec4:
    return 190.0f;
  case mg::ValueType::Float:
    break;
  }

  return 58.0f;
}

ImU32 typeColor(mg::ValueType type)
{
  switch (type)
  {
  case mg::ValueType::Vec2:
    return IM_COL32(132, 200, 132, 255);
  case mg::ValueType::Vec3:
    return IM_COL32(230, 200, 96, 255);
  case mg::ValueType::Vec4:
    return IM_COL32(204, 146, 224, 255);
  case mg::ValueType::Float:
    break;
  }

  return IM_COL32(168, 168, 168, 255);
}

ImU32 categoryColor(const std::string& category)
{
  if (category == "Output")
    return IM_COL32(96, 128, 90, 255);

  if (category == "Input")
    return IM_COL32(70, 98, 132, 255);

  if (category == "Math")
    return IM_COL32(84, 84, 112, 255);

  if (category == "Vector")
    return IM_COL32(112, 92, 68, 255);

  if (category == "UV")
    return IM_COL32(64, 108, 102, 255);

  if (category == "Effects")
    return IM_COL32(118, 76, 88, 255);

  return IM_COL32(80, 80, 80, 255);
}

// An input's own value as it reads on the node: "0.5", "1, 0, 0".
std::string valueText(const glm::vec4& value, mg::ValueType type)
{
  std::string out;
  char buffer[32];

  for (int i = 0; i < mg::width(type); ++i)
  {
    std::snprintf(buffer, sizeof(buffer), "%.3g", value[i]);
    out += (i ? ", " : "") + std::string(buffer);
  }

  return out;
}

// A texture node's image, by its file name.
std::string textureLabel(const mg::Node& node, const sfs::AssetDatabase* assets)
{
  if (node.texture.empty())
    return "No image";

  constexpr std::string_view PREFIX = "asset:";

  if (assets && node.texture.starts_with(PREFIX))
    if (const auto id = sfs::AssetId::parse(
            std::string_view(node.texture).substr(PREFIX.size())))
      if (const sfs::AssetRecord* record = assets->find(*id))
        return record->path.stem().string();

  return "Missing image";
}

// A global parameter node's parameter, by its name.
std::string globalLabel(const mg::Node& node)
{
  return node.global.empty() ? "No parameter" : node.global;
}

mg::ValueType dynamicOf(const mg::CompiledGraph& compiled, mg::NodeId node)
{
  const auto it = compiled.dynamicTypes.find(node);
  return it != compiled.dynamicTypes.end() ? it->second : mg::ValueType::Float;
}

const mg::CompileError* errorOf(const mg::CompiledGraph& compiled,
                                mg::NodeId node)
{
  for (const mg::CompileError& error : compiled.errors)
    if (error.node == node)
      return &error;

  return nullptr;
}

bool contains(const std::string& text, const std::string& filter)
{
  const auto lower = [](std::string s)
  {
    std::transform(s.begin(),
                   s.end(),
                   s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
  };
  return lower(text).find(lower(filter)) != std::string::npos;
}

} // namespace

// --- layout ------------------------------------------------------------------

MaterialGraphCanvas::Layout
MaterialGraphCanvas::layout(const mg::Node& node,
                            const mg::CompiledGraph& compiled,
                            const sfs::AssetDatabase* assets) const
{
  const board::BoxStyle& style = m_board.style();
  const float pad = style.padding;
  const float row = style.lineHeight;
  Layout out;
  out.min = {node.position.x, node.position.y};
  const mg::NodeType* type = mg::findNodeType(node.type);
  const auto textWidth = [](const std::string& text)
  { return ImGui::CalcTextSize(text.c_str()).x; };

  if (!type)
  {
    out.max = out.min +
              ImVec2(std::max(style.minWidth, textWidth(node.type) + 2 * pad),
                     style.headerHeight + row);
    return out;
  }

  const mg::ValueType dynamic = dynamicOf(compiled, node.id);
  float inputsWidth = 0.0f, outputsWidth = 0.0f;

  // Each input's value (or what it reads) beside its name, while unlinked.
  // Room is kept for it while linked too, so linking doesn't resize the node.
  for (uint32_t i = 0; i < type->inputs.size(); ++i)
  {
    const mg::InputPin& pin = type->inputs[i];
    float extra = 0.0f;

    if (pin.fallback != mg::Fallback::Value)
      extra = textWidth(mg::fallbackName(pin.fallback));
    else if (pin.color)
      extra = SWATCH_WIDTH;
    else if (i < node.inputs.size())
      extra = std::max(
          fieldWidth(type->inputType(i, dynamic)),
          textWidth(valueText(node.inputs[i], type->inputType(i, dynamic))));

    inputsWidth = std::max(inputsWidth, textWidth(pin.name) + 8.0f + extra);
  }

  for (const mg::OutputPin& pin : type->outputs)
    outputsWidth = std::max(outputsWidth, textWidth(pin.name));

  float propertyWidth = 0.0f;

  if (type->property == mg::Property::Texture)
    propertyWidth = textWidth(textureLabel(node, assets)) +
                    (node.dataTexture ? textWidth(" (data)") : 0.0f);
  else if (type->property == mg::Property::Number)
    propertyWidth =
        std::max(fieldWidth(mg::ValueType::Float) + 30.0f,
                 textWidth(valueText(node.value, mg::ValueType::Float)));
  else if (type->property == mg::Property::Global)
    propertyWidth =
        std::max(fieldWidth(mg::ValueType::Vec4), textWidth(globalLabel(node)));

  const float width =
      std::max({style.minWidth,
                textWidth(type->label) + 2 * pad,
                inputsWidth + outputsWidth + 3 * pad + 2 * PIN_RADIUS,
                propertyWidth + 2 * pad});
  const int propertyRows = type->property == mg::Property::None ? 0 : 1;
  const std::size_t rows = std::max(type->inputs.size(), type->outputs.size());
  out.max =
      out.min +
      ImVec2(width,
             style.headerHeight +
                 (propertyRows + static_cast<float>(rows)) * row + pad * 0.5f);
  out.pinsTop =
      out.min.y + style.headerHeight + propertyRows * row + row * 0.5f;
  out.inputs = static_cast<uint32_t>(type->inputs.size());
  out.outputs = static_cast<uint32_t>(type->outputs.size());
  return out;
}

void MaterialGraphCanvas::layoutNodes(const Graph& graph,
                                      const mg::CompiledGraph& compiled,
                                      const sfs::AssetDatabase* assets)
{
  // Selected nodes draw (and are hit) last: on top.
  const float row = m_board.style().lineHeight;
  m_boxes.clear();
  m_firstPins.clear();
  m_pins.clear();

  for (int pass = 0; pass < 2; ++pass)
    for (const mg::Node& node : graph.nodes)
      if (m_board.isSelected(node.id) == (pass == 1))
      {
        const Layout nodeLayout = layout(node, compiled, assets);
        m_firstPins.push_back(m_pins.size());

        for (uint32_t i = 0; i < nodeLayout.inputs; ++i)
          m_pins.push_back(
              {{node.id, i, false},
               ImVec2(nodeLayout.min.x, nodeLayout.pinsTop + i * row)});

        for (uint32_t i = 0; i < nodeLayout.outputs; ++i)
          m_pins.push_back(
              {{node.id, i, true},
               ImVec2(nodeLayout.max.x, nodeLayout.pinsTop + i * row)});

        m_boxes.push_back({node.id, nodeLayout.min, nodeLayout.max});
      }
}

MaterialGraphCanvas::NodeId MaterialGraphCanvas::nodeAt(ImVec2 screen) const
{
  const board::Box* box =
      board::boxAt(m_boxes, m_board.view().toCanvas(screen));
  return box ? static_cast<NodeId>(box->id) : 0;
}

std::optional<MaterialGraphCanvas::Pin>
MaterialGraphCanvas::pinAt(ImVec2 screen) const
{
  const board::View& view = m_board.view();
  const float reach = std::max(PIN_RADIUS * 2.0f * view.zoom, 7.0f);

  for (auto it = m_pins.rbegin(); it != m_pins.rend(); ++it)
    if (ImLengthSqr(view.toScreen(it->second) - screen) <= reach * reach)
      return it->first;

  return std::nullopt;
}

ImVec2 MaterialGraphCanvas::placement() const
{
  const board::View& view = m_board.view();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const ImRect rect(view.origin, view.origin + view.size);
  return view.toCanvas(rect.Contains(mouse) ? mouse : rect.GetCenter());
}

// --- drawing
// -------------------------------------------------------------------

void MaterialGraphCanvas::draw(Graph& graph,
                               const mg::CompiledGraph& compiled,
                               const sfs::AssetDatabase* assets)
{
  // The nodes' fields, drawn after, take the mouse where they are.
  if (!m_board.begin("canvas"))
    return;

  // Laid out for the mouse's hit tests, then again for drawing what it did
  // (so moved nodes don't lag a frame behind).
  layoutNodes(graph, compiled, assets);
  m_board.applyFrame(m_boxes);
  acceptTextureDrop(graph, assets);
  handleMouse(graph);
  layoutNodes(graph, compiled, assets);

  ImDrawList* draw = m_board.beginDraw();
  drawLinks(draw, graph, compiled);

  for (std::size_t i = 0; i < m_boxes.size(); ++i)
    if (mg::Node* node = graph.find(static_cast<NodeId>(m_boxes[i].id)))
    {
      drawNode(draw, *node, i, graph, compiled, assets);

      // Right after its node, so a node on top covers the one below's.
      if (fieldsShown())
        drawFields(*node, i, graph, compiled);
    }

  if (m_board.gesture() == board::Gesture::Custom)
  {
    // The link being dragged, from its pin to the mouse.
    const auto it =
        std::find_if(m_pins.begin(),
                     m_pins.end(),
                     [&](const auto& p) { return p.first == m_linkFrom; });

    if (it != m_pins.end())
    {
      ImVec2 a = m_board.view().toScreen(it->second);
      ImVec2 b = ImGui::GetIO().MousePos;

      if (!m_linkFrom.output)
        std::swap(a, b);

      m_board.drawScreenLink(
          draw, a, b, {.color = theme::kAccent, .minWidth = 1.5f});
    }
  }

  m_board.endDraw(draw);

  drawAddMenu(graph);
  drawNodeMenu(graph);
  drawColorPicker(graph);
}

void MaterialGraphCanvas::drawColorPicker(Graph& graph)
{
  if (std::exchange(m_openPicker, false))
    ImGui::OpenPopup("##colorPicker");

  if (!ImGui::BeginPopup("##colorPicker"))
    return;

  mg::Node* node = graph.find(m_pickerNode);
  const mg::NodeType* type = node ? mg::findNodeType(node->type) : nullptr;
  glm::vec4* value = nullptr;

  if (node && m_pickerInput < 0)
    value = &node->value;
  else if (node && type &&
           static_cast<std::size_t>(m_pickerInput) < node->inputs.size())
    value = &node->inputs[m_pickerInput];

  if (!value)
  {
    // Its node went (an undo): nothing to pick for.
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }

  // A Colour node's colour has an alpha (its Alpha output); inputs don't.
  if (m_pickerInput < 0)
    ImGui::ColorPicker4("##picker", &value->x, ImGuiColorEditFlags_AlphaBar);
  else
    ImGui::ColorPicker3("##picker", &value->x);

  *value = glm::clamp(*value, glm::vec4(0.0f), glm::vec4(1.0f));
  ImGui::EndPopup();
}

void MaterialGraphCanvas::drawFields(mg::Node& node,
                                     std::size_t index,
                                     const Graph& graph,
                                     const mg::CompiledGraph& compiled)
{
  const mg::NodeType* type = mg::findNodeType(node.type);

  if (!type)
    return;

  const board::View& view = m_board.view();
  const board::BoxStyle& style = m_board.style();
  const float zoom = view.zoom;
  const float pad = style.padding;
  const board::Box& box = m_boxes[index];
  const std::size_t firstPin = m_firstPins[index];
  const mg::ValueType dynamic = dynamicOf(compiled, node.id);

  // Widgets at the canvas's zoom: the font and spacing scaled with it, frames
  // a row high.
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, 1.0f) * zoom);
  ImGui::PushStyleVar(
      ImGuiStyleVar_ItemInnerSpacing, ImVec2(2.0f, 2.0f) * zoom);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * zoom);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 6.0f * zoom);
  ImGui::PushID(static_cast<int>(node.id));
  const float frameHeight = ImGui::GetFrameHeight();
  const ImVec2 min = view.toScreen(box.min), max = view.toScreen(box.max);

  // Editing a node's value selects it.
  const auto selectIfEdited = [&]
  {
    if (ImGui::IsItemActivated())
      m_board.select(node.id);
  };
  const auto colorField =
      [&](const char* id, glm::vec4& value, int input, ImVec2 fieldSize)
  {
    if (ImGui::ColorButton(
            id,
            ImVec4(value.r, value.g, value.b, 1.0f),
            ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
            fieldSize))
    {
      m_pickerNode = node.id;
      m_pickerInput = input;
      m_openPicker = true;
      m_board.select(node.id);
    }
  };

  // The property row: a Colour's colour, a Number's number, a Global
  // Parameter's name.
  const float propertyTop = min.y + style.headerHeight * zoom;
  const float rowWidth = max.x - min.x - 2.0f * pad * zoom;
  const float propertyY =
      propertyTop + (style.lineHeight * zoom - frameHeight) * 0.5f;

  if (type->property == mg::Property::Color)
  {
    ImGui::SetCursorScreenPos(ImVec2(min.x + pad * zoom, propertyY));
    colorField("##value", node.value, -1, ImVec2(rowWidth, frameHeight));
  }
  else if (type->property == mg::Property::Number)
  {
    ImGui::SetCursorScreenPos(ImVec2(min.x + pad * zoom, propertyY));
    ImGui::SetNextItemWidth(rowWidth);
    ImGui::DragFloat("##value", &node.value.x, 0.01f, 0.0f, 0.0f, "%.3g");
    selectIfEdited();
  }
  else if (type->property == mg::Property::Global)
  {
    ImGui::SetCursorScreenPos(ImVec2(min.x + pad * zoom, propertyY));
    ImGui::SetNextItemWidth(rowWidth);
    widgets::inputTextOnCommit("##global", node.global);
    selectIfEdited();
    ImGui::SetItemTooltip("The global shader parameter's name, as the scene "
                          "has it (Scene Data > Shader Globals)");
  }

  // Each unlinked input's own value, after its name.
  for (uint32_t i = 0; i < type->inputs.size() && i < node.inputs.size(); ++i)
  {
    const mg::InputPin& pin = type->inputs[i];

    if (pin.fallback != mg::Fallback::Value || graph.linkInto(node.id, i))
      continue;

    ImGui::PushID(static_cast<int>(i));
    const float nameWidth = ImGui::CalcTextSize(pin.name.c_str()).x;
    const ImVec2 centre = view.toScreen(m_pins[firstPin + i].second);
    ImGui::SetCursorScreenPos(
        ImVec2(min.x + (pad + PIN_RADIUS + 8.0f) * zoom + nameWidth,
               centre.y - frameHeight * 0.5f));
    glm::vec4& value = node.inputs[i];
    const mg::ValueType inputType = type->inputType(i, dynamic);

    if (pin.color)
      colorField("##value",
                 value,
                 static_cast<int>(i),
                 ImVec2(SWATCH_WIDTH * zoom, frameHeight));
    else
    {
      ImGui::SetNextItemWidth(fieldWidth(inputType) * zoom);

      switch (inputType)
      {
      case mg::ValueType::Float:
        if (pin.unit)
          ImGui::SliderFloat("##value", &value.x, 0.0f, 1.0f, "%.2f");
        else
          ImGui::DragFloat("##value", &value.x, 0.01f, 0.0f, 0.0f, "%.3g");

        break;
      case mg::ValueType::Vec2:
        ImGui::DragFloat2("##value", &value.x, 0.01f, 0.0f, 0.0f, "%.3g");
        break;
      case mg::ValueType::Vec3:
        ImGui::DragFloat3("##value", &value.x, 0.01f, 0.0f, 0.0f, "%.3g");
        break;
      case mg::ValueType::Vec4:
        ImGui::DragFloat4("##value", &value.x, 0.01f, 0.0f, 0.0f, "%.3g");
        break;
      }

      selectIfEdited();
    }

    ImGui::PopID();
  }

  ImGui::PopID();
  ImGui::PopStyleVar(4);
  ImGui::PopFont();
}

void MaterialGraphCanvas::drawLinks(ImDrawList* draw,
                                    const Graph& graph,
                                    const mg::CompiledGraph& compiled) const
{
  const auto pinPosition = [&](const Pin& pin) -> std::optional<ImVec2>
  {
    for (const auto& [p, position] : m_pins)
      if (p == pin)
        return position;

    return std::nullopt;
  };

  for (const mg::Link& link : graph.links)
  {
    const auto from = pinPosition({link.fromNode, link.fromOutput, true});
    const auto to = pinPosition({link.toNode, link.toInput, false});

    if (!from || !to)
      continue;

    const mg::Node* source = graph.find(link.fromNode);
    const mg::NodeType* type =
        source ? mg::findNodeType(source->type) : nullptr;
    const ImU32 color =
        type ? typeColor(type->outputType(
                   link.fromOutput, dynamicOf(compiled, link.fromNode)))
             : IM_COL32_WHITE;
    m_board.drawLink(draw, *from, *to, {.color = color});
  }
}

void MaterialGraphCanvas::drawNode(ImDrawList* draw,
                                   const mg::Node& node,
                                   std::size_t index,
                                   const Graph& graph,
                                   const mg::CompiledGraph& compiled,
                                   const sfs::AssetDatabase* assets) const
{
  const mg::NodeType* type = mg::findNodeType(node.type);
  const mg::CompileError* error = errorOf(compiled, node.id);
  const board::Box& box = m_boxes[index];
  const std::string unknown = type ? std::string() : node.type + " (unknown)";

  // Body, header and outline.
  board::BoxLook look;
  look.title = type ? std::string_view(type->label) : unknown;
  look.header =
      type ? categoryColor(type->category) : IM_COL32(110, 60, 60, 255);
  look.emphasis = error ? ERROR_COLOR : 0;
  look.selected = m_board.isSelected(node.id);

  if (!m_board.drawBox(draw, box, look))
    return;

  const board::View& view = m_board.view();
  const board::BoxStyle& style = m_board.style();
  const float zoom = view.zoom;
  const float pad = style.padding;
  const float row = style.lineHeight;
  const ImVec2 min = view.toScreen(box.min), max = view.toScreen(box.max);
  const float fontSize = m_board.fontSize();
  const bool showText = m_board.textShown();
  ImFont* font = ImGui::GetFont();
  const auto text = [&](ImVec2 at, ImU32 color, const std::string& value)
  {
    if (showText)
      draw->AddText(font, fontSize, at, color, value.c_str());
  };
  const float textOffset = (row * zoom - fontSize) * 0.5f;

  if (error && ImGui::IsItemHovered() &&
      ImRect(min, max).Contains(ImGui::GetIO().MousePos) && !m_board.busy())
    ImGui::SetTooltip("%s", error->message.c_str());

  if (!type)
    return;

  // The property row: the node's colour, number or image.
  const std::size_t firstPin = m_firstPins[index];
  const mg::ValueType dynamic = dynamicOf(compiled, node.id);
  float y = min.y + style.headerHeight * zoom;

  if (type->property != mg::Property::None)
  {
    const ImVec2 rowMin(min.x + pad * zoom, y + 3.0f * zoom);
    const ImVec2 rowMax(max.x - pad * zoom, y + (row - 3.0f) * zoom);

    // A Colour's colour and a Number's number are fields while those show.
    if (type->property == mg::Property::Color && !fieldsShown())
    {
      draw->AddRectFilled(rowMin,
                          rowMax,
                          ImGui::ColorConvertFloat4ToU32(ImVec4(
                              node.value.r, node.value.g, node.value.b, 1.0f)),
                          3.0f * zoom);
      draw->AddRect(rowMin, rowMax, IM_COL32(0, 0, 0, 120), 3.0f * zoom);
    }
    else if (type->property == mg::Property::Number && !fieldsShown())
      text(ImVec2(rowMin.x, y + textOffset),
           IM_COL32(220, 222, 216, 255),
           valueText(node.value, mg::ValueType::Float));
    else if (type->property == mg::Property::Global && !fieldsShown())
      text(ImVec2(rowMin.x, y + textOffset),
           IM_COL32(220, 222, 216, 255),
           globalLabel(node));
    else if (type->property == mg::Property::Texture)
      text(ImVec2(rowMin.x, y + textOffset),
           IM_COL32(220, 222, 216, 255),
           textureLabel(node, assets) + (node.dataTexture ? " (data)" : ""));

    y += row * zoom;
  }

  // Inputs on the left, each with its own value while unlinked; outputs on
  // the right.
  for (uint32_t i = 0; i < type->inputs.size(); ++i)
  {
    const mg::InputPin& pin = type->inputs[i];
    const ImVec2 centre = view.toScreen(m_pins[firstPin + i].second);
    const bool linked = graph.linkInto(node.id, i) != nullptr;
    const ImU32 color = typeColor(type->inputType(i, dynamic));

    if (linked)
      draw->AddCircleFilled(centre, PIN_RADIUS * zoom, color);
    else
      draw->AddCircle(
          centre, PIN_RADIUS * zoom, color, 0, std::max(1.5f * zoom, 1.0f));

    const ImVec2 at(
        min.x + (pad + PIN_RADIUS) * zoom, centre.y - fontSize * 0.5f);
    text(at, IM_COL32(214, 216, 210, 255), pin.name);

    if (linked)
      continue;

    const float nameWidth =
        showText
            ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, pin.name.c_str()).x
            : 0.0f;
    const ImVec2 valueAt = at + ImVec2(nameWidth + 8.0f * zoom, 0.0f);

    if (pin.fallback != mg::Fallback::Value)
      text(valueAt,
           IM_COL32(150, 154, 148, 255),
           mg::fallbackName(pin.fallback));
    else if (fieldsShown())
      continue; // its field shows the value
    else if (i < node.inputs.size() && pin.color)
    {
      const ImVec2 swatch(12.0f * zoom, 12.0f * zoom);
      const ImVec2 swatchMin =
          valueAt + ImVec2(0.0f, (fontSize - swatch.y) * 0.5f);
      const glm::vec4& v = node.inputs[i];
      draw->AddRectFilled(
          swatchMin,
          swatchMin + swatch,
          ImGui::ColorConvertFloat4ToU32(ImVec4(v.r, v.g, v.b, 1.0f)),
          2.0f * zoom);
    }
    else if (i < node.inputs.size())
      text(valueAt,
           IM_COL32(150, 154, 148, 255),
           valueText(node.inputs[i], type->inputType(i, dynamic)));
  }

  const std::size_t firstOutput = firstPin + type->inputs.size();

  for (uint32_t i = 0; i < type->outputs.size(); ++i)
  {
    const ImVec2 centre = view.toScreen(m_pins[firstOutput + i].second);
    const ImU32 color = typeColor(type->outputType(i, dynamic));
    const bool linked = std::any_of(
        graph.links.begin(),
        graph.links.end(),
        [&](const mg::Link& link)
        { return link.fromNode == node.id && link.fromOutput == i; });

    if (linked)
      draw->AddCircleFilled(centre, PIN_RADIUS * zoom, color);
    else
      draw->AddCircle(
          centre, PIN_RADIUS * zoom, color, 0, std::max(1.5f * zoom, 1.0f));

    if (showText)
    {
      const float width =
          font->CalcTextSizeA(
                  fontSize, FLT_MAX, 0.0f, type->outputs[i].name.c_str())
              .x;
      text(ImVec2(max.x - (pad + PIN_RADIUS) * zoom - width,
                  centre.y - fontSize * 0.5f),
           IM_COL32(214, 216, 210, 255),
           type->outputs[i].name);
    }
  }
}

bool MaterialGraphCanvas::fieldsShown() const
{
  return m_board.zoom() >= FIELDS_ZOOM;
}

// --- mouse -------------------------------------------------------------------

void MaterialGraphCanvas::acceptTextureDrop(Graph& graph,
                                            const sfs::AssetDatabase* assets)
{
  if (!assets || !ImGui::BeginDragDropTarget())
    return;

  const sfs::AssetRecord* dragged =
      assetDrop::draggedAsset(ImGui::GetDragDropPayload(), *assets);

  if (dragged && dragged->kind == sfs::AssetKind::Texture &&
      ImGui::AcceptDragDropPayload(assetDrop::kAssetPayload))
  {
    const ImVec2 at = m_board.view().toCanvas(ImGui::GetIO().MousePos);
    const NodeId node = graph.add("texture", {at.x, at.y});
    graph.find(node)->texture = "asset:" + dragged->id.toString();
    m_board.select(node);
  }

  ImGui::EndDragDropTarget();
}

void MaterialGraphCanvas::finishLink(Graph& graph, ImVec2 mouse)
{
  std::optional<Pin> target = pinAt(mouse);

  if (!target)
  {
    // Dropped on a node: its first pin of the other kind that fits.
    if (const NodeId node = nodeAt(mouse); node && node != m_linkFrom.node)
    {
      const mg::Node* other = graph.find(node);
      const mg::NodeType* type =
          other ? mg::findNodeType(other->type) : nullptr;

      if (type && m_linkFrom.output && !type->inputs.empty())
      {
        uint32_t chosen = 0;

        for (uint32_t i = 0; i < type->inputs.size(); ++i)
          if (!graph.linkInto(node, i))
          {
            chosen = i;
            break;
          }

        target = Pin{node, chosen, false};
      }
      else if (type && !m_linkFrom.output && !type->outputs.empty())
        target = Pin{node, 0, true};
    }
    else if (!node)
    {
      // Dropped on empty space: a link picked up off an input is just gone
      // (unlinked); a new one adds a node there, linked.
      if (m_linkPickedUp)
        return;

      m_addLink = m_linkFrom;
      m_openAddMenu = true;
      return;
    }
  }

  if (!target || target->output == m_linkFrom.output ||
      target->node == m_linkFrom.node)
    return;

  const Pin& from = m_linkFrom.output ? m_linkFrom : *target;
  const Pin& to = m_linkFrom.output ? *target : m_linkFrom;
  graph.connect(from.node, from.index, to.node, to.index); // refuses loops
}

void MaterialGraphCanvas::handleMouse(Graph& graph)
{
  const board::Input input = m_board.handleMouse(
      m_boxes, [&](ImVec2 mouse) { return pressPin(graph, mouse); });

  for (const board::BoxMove& move : input.moved)
    if (mg::Node* node = graph.find(static_cast<NodeId>(move.id)))
      node->position = {move.min.x, move.min.y};

  if (input.gestureEnded)
    finishLink(graph, ImGui::GetIO().MousePos);

  // A right-click: the node's menu, or Add Node.
  if (!input.rightClicked)
    return;

  if (input.rightClickedBox)
  {
    m_menuNode = static_cast<NodeId>(input.rightClickedBox);
    ImGui::OpenPopup("##nodeMenu");
    return;
  }

  m_addLink.reset();
  m_openAddMenu = true;
}

board::Press MaterialGraphCanvas::pressPin(Graph& graph, ImVec2 mouse)
{
  const std::optional<Pin> pin = pinAt(mouse);

  if (!pin)
    return board::Press::Unhandled;

  if (ImGui::GetIO().KeyAlt)
  {
    // Cut the pin's links.
    if (pin->output)
      std::erase_if(graph.links,
                    [&](const mg::Link& link) {
                      return link.fromNode == pin->node &&
                             link.fromOutput == pin->index;
                    });
    else
      graph.disconnect(pin->node, pin->index);

    return board::Press::Handled;
  }

  if (const mg::Link* link =
          pin->output ? nullptr : graph.linkInto(pin->node, pin->index))
  {
    // Pick the link up from this input, to put it somewhere else.
    m_linkFrom = {link->fromNode, link->fromOutput, true};
    m_linkPickedUp = true;
    graph.disconnect(pin->node, pin->index);
    return board::Press::Gesture;
  }

  m_linkFrom = *pin;
  m_linkPickedUp = false;
  return board::Press::Gesture;
}

// --- menus -------------------------------------------------------------------

void MaterialGraphCanvas::addNode(Graph& graph, const mg::NodeType& type)
{
  const NodeId node = graph.add(type.id, {m_addAt.x, m_addAt.y});

  if (!node)
    return;

  m_board.select(node);

  if (!m_addLink)
    return;

  // Link it to the pin the link was dragged from: at its first pin of the
  // other kind, preferring one of the same type (any links, converting).
  const Pin from = *m_addLink;
  m_addLink.reset();
  const mg::Node* source = graph.find(from.node);
  const mg::NodeType* sourceType =
      source ? mg::findNodeType(source->type) : nullptr;

  if (!sourceType)
    return;

  if (from.output)
  {
    if (type.inputs.empty())
      return;

    const mg::ValueType wanted =
        sourceType->outputType(from.index, mg::ValueType::Float);
    uint32_t chosen = 0;

    for (uint32_t i = 0; i < type.inputs.size(); ++i)
      if (!type.inputs[i].type || *type.inputs[i].type == wanted)
      {
        chosen = i;
        break;
      }

    graph.connect(from.node, from.index, node, chosen);
  }
  else
  {
    if (type.outputs.empty())
      return;

    const auto wanted = sourceType->inputs[from.index].type;
    uint32_t chosen = 0;

    for (uint32_t i = 0; i < type.outputs.size(); ++i)
      if (!wanted || !type.outputs[i].type || *type.outputs[i].type == *wanted)
      {
        chosen = i;
        break;
      }

    graph.connect(node, chosen, from.node, from.index);
  }

  // Move it so the link reads left to right.
  if (!from.output)
    graph.find(node)->position.x -= 180.0f;
}

void MaterialGraphCanvas::drawAddMenu(Graph& graph)
{
  if (std::exchange(m_openAddMenu, false))
  {
    m_addAt = placement();
    m_addFilter.clear();
    m_focusFilter = true;
    ImGui::OpenPopup("##addNode");
  }

  if (!ImGui::BeginPopup("##addNode"))
    return;

  ImGui::TextDisabled("Add Node");

  if (std::exchange(m_focusFilter, false))
    ImGui::SetKeyboardFocusHere();

  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "%s", m_addFilter.c_str());
  ImGui::SetNextItemWidth(220.0f);
  const bool entered =
      ImGui::InputTextWithHint("##filter",
                               "Search",
                               buffer,
                               sizeof(buffer),
                               ImGuiInputTextFlags_EnterReturnsTrue);
  m_addFilter = buffer;

  const auto item = [&](const mg::NodeType& type)
  {
    if (ImGui::MenuItem(type.label.c_str()))
    {
      addNode(graph, type);
      ImGui::CloseCurrentPopup();
    }

    ImGui::SetItemTooltip("%s", type.description.c_str());
  };

  if (m_addFilter.empty())
  {
    // By category, in the order they first appear.
    std::vector<std::string> categories;

    for (const mg::NodeType& type : mg::nodeTypes())
      if (type.id != mg::kOutputType &&
          std::find(categories.begin(), categories.end(), type.category) ==
              categories.end())
        categories.push_back(type.category);

    for (const std::string& category : categories)
      if (ImGui::BeginMenu(category.c_str()))
      {
        for (const mg::NodeType& type : mg::nodeTypes())
          if (type.category == category && type.id != mg::kOutputType)
            item(type);

        ImGui::EndMenu();
      }
  }
  else
  {
    const mg::NodeType* first = nullptr;

    for (const mg::NodeType& type : mg::nodeTypes())
      if (type.id != mg::kOutputType && (contains(type.label, m_addFilter) ||
                                         contains(type.category, m_addFilter)))
      {
        first = first ? first : &type;
        item(type);
      }

    if (!first)
      ImGui::TextDisabled("No nodes match");

    if (entered && first)
    {
      addNode(graph, *first);
      ImGui::CloseCurrentPopup();
    }
  }

  ImGui::EndPopup();
}

void MaterialGraphCanvas::drawNodeMenu(Graph& graph)
{
  if (!ImGui::BeginPopup("##nodeMenu"))
    return;

  const mg::Node* node = graph.find(m_menuNode);

  if (!node)
  {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }

  const bool output = node->type == mg::kOutputType;

  if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !output))
    duplicateSelection(graph);

  if (ImGui::MenuItem("Copy", "Ctrl+C"))
    copySelection(graph);

  if (ImGui::MenuItem("Cut Links"))
    std::erase_if(
        graph.links,
        [&](const mg::Link& link)
        { return link.fromNode == m_menuNode || link.toNode == m_menuNode; });

  ImGui::Separator();

  if (ImGui::MenuItem("Delete", "Del", false, !output))
    deleteSelection(graph);

  if (output)
    ImGui::SetItemTooltip("Every graph has its Material Output");

  ImGui::EndPopup();
}

// --- commands
// ------------------------------------------------------------------

void MaterialGraphCanvas::deleteSelection(Graph& graph)
{
  for (const board::BoxId box : m_board.selection())
  {
    const auto id = static_cast<NodeId>(box);

    if (const mg::Node* node = graph.find(id);
        node && node->type != mg::kOutputType)
      graph.remove(id);
  }

  m_board.deselectIf([&](board::BoxId id)
                     { return !graph.find(static_cast<NodeId>(id)); });
}

namespace
{
// The selected nodes and the links between them, as JSON; with
// `withOutput`, the Material Output too if it's selected (copying a whole
// graph, to paste into another).
std::string selectionJson(const mg::Graph& graph,
                          const board::Canvas& board,
                          bool withOutput)
{
  mg::Graph copy;

  for (const mg::Node& node : graph.nodes)
    if (board.isSelected(node.id) &&
        (withOutput || node.type != mg::kOutputType))
      copy.nodes.push_back(node);

  for (const mg::Link& link : graph.links)
    if (copy.find(link.fromNode) && copy.find(link.toNode))
      copy.links.push_back(link);

  if (copy.nodes.empty())
    return {};

  return nlohmann::json{{CLIPBOARD_KEY, copy.toJson()}}.dump();
}
} // namespace

void MaterialGraphCanvas::copySelection(const Graph& graph) const
{
  const std::string json = selectionJson(graph, m_board, true);

  if (!json.empty())
    ImGui::SetClipboardText(json.c_str());
}

void MaterialGraphCanvas::duplicateSelection(Graph& graph)
{
  ImVec2 topLeft(FLT_MAX, FLT_MAX);

  for (const board::BoxId id : m_board.selection())
    if (const mg::Node* node = graph.find(static_cast<NodeId>(id));
        node && node->type != mg::kOutputType)
      topLeft = ImMin(topLeft, ImVec2(node->position.x, node->position.y));

  const std::string json = selectionJson(graph, m_board, false);

  if (!json.empty())
    pasteJson(graph, json, topLeft + ImVec2(40.0f, 40.0f));
}

void MaterialGraphCanvas::paste(Graph& graph)
{
  const char* text = ImGui::GetClipboardText();

  if (!text)
    return;

  pasteJson(graph, text, placement());
}

void MaterialGraphCanvas::pasteJson(Graph& graph,
                                    const std::string& text,
                                    ImVec2 at)
{
  const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);

  if (!json.is_object() || !json.contains(CLIPBOARD_KEY))
    return;

  const std::optional<Graph> pasted = Graph::fromJson(json[CLIPBOARD_KEY]);

  if (!pasted || pasted->nodes.empty())
    return;

  // With a Material Output of its own (a whole graph, such as a recipe from
  // the docs), it joins this graph's: laid out around it (below the graph's
  // own nodes, if they're in the way), linked into it, and giving it its
  // values. Otherwise the nodes go at `at`.
  const mg::Node* pastedOutput = pasted->find(pasted->output());
  mg::Node* output = graph.find(graph.output());
  ImVec2 offset;

  if (pastedOutput && output)
  {
    offset = ImVec2(output->position.x - pastedOutput->position.x,
                    output->position.y - pastedOutput->position.y);
    // Clear of the graph's own nodes: below them, if they'd overlap.
    const mg::CompiledGraph none;
    ImRect pastedBounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));

    for (const mg::Node& node : pasted->nodes)
      if (node.id != pastedOutput->id)
      {
        const Layout nodeLayout = layout(node, none, nullptr);
        pastedBounds.Add(
            ImRect(nodeLayout.min + offset, nodeLayout.max + offset));
      }

    ImRect ownBounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));

    for (const board::Box& box : m_boxes)
      if (box.id != output->id)
        ownBounds.Add(ImRect(box.min, box.max));

    if (pastedBounds.Min.x <= pastedBounds.Max.x &&
        ownBounds.Min.x <= ownBounds.Max.x && pastedBounds.Overlaps(ownBounds))
      offset.y += ownBounds.Max.y - pastedBounds.Min.y + 60.0f;
  }
  else
  {
    ImVec2 topLeft(FLT_MAX, FLT_MAX);

    for (const mg::Node& node : pasted->nodes)
      topLeft = ImMin(topLeft, ImVec2(node.position.x, node.position.y));

    offset = at - topLeft;
  }

  std::unordered_map<NodeId, NodeId> ids;
  m_board.clearSelection();

  for (mg::Node node : pasted->nodes)
  {
    if (node.type == mg::kOutputType)
    {
      if (output && node.id == pastedOutput->id)
      {
        ids[node.id] = output->id;
        output->inputs = node.inputs;
      }

      continue; // a graph has one Material Output
    }

    ids[node.id] = graph.nextId;
    node.id = graph.nextId++;
    node.position += glm::vec2(offset.x, offset.y);
    m_board.selectAlso(node.id);
    graph.nodes.push_back(std::move(node));
  }

  for (const mg::Link& link : pasted->links)
    if (ids.contains(link.fromNode) && ids.contains(link.toNode))
      graph.connect(
          ids[link.fromNode], link.fromOutput, ids[link.toNode], link.toInput);
}

void MaterialGraphCanvas::selectAll(const Graph& graph)
{
  m_board.clearSelection();

  for (const mg::Node& node : graph.nodes)
    m_board.selectAlso(node.id);
}
