#include "materials/materialGraphCanvas.h"

#include "assets/assetDrop.h"
#include "widgets/theme.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/rendering/3d/materialNodes.h"

#include "glm/glm/common.hpp"
#include "imgui_internal.h" // ImRect, ImLengthSqr

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace mg = sfs::materialGraph;

namespace
{

// Sizes in canvas units (screen pixels at zoom 1).
constexpr float kHeader = 24.0f;
constexpr float kRow = 21.0f;
constexpr float kPad = 9.0f;
constexpr float kPinRadius = 5.0f;
constexpr float kMinWidth = 140.0f;
constexpr float kGrid = 32.0f;
constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 2.5f;
constexpr float kFieldsZoom = 0.6f; // below this, values show as text
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
constexpr float kSwatchWidth = 20.0f;

constexpr const char* kClipboardKey = "sfsMaterialNodes";

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

constexpr ImU32 kErrorColor = IM_COL32(230, 96, 86, 255);

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
  constexpr std::string_view prefix = "asset:";
  if (assets && node.texture.starts_with(prefix))
    if (const auto id = sfs::AssetId::parse(
            std::string_view(node.texture).substr(prefix.size())))
      if (const sfs::AssetRecord* record = assets->find(*id))
        return record->path.stem().string();
  return "Missing image";
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

ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
ImVec2 operator*(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

} // namespace

// --- layout ------------------------------------------------------------------

MaterialGraphCanvas::Layout
MaterialGraphCanvas::layout(const mg::Node& node,
                            const mg::CompiledGraph& compiled,
                            const sfs::AssetDatabase* assets) const
{
  Layout out;
  out.min = {node.position.x, node.position.y};
  const mg::NodeType* type = mg::findNodeType(node.type);
  const auto textWidth = [](const std::string& text)
  { return ImGui::CalcTextSize(text.c_str()).x; };
  if (!type)
  {
    out.max =
        out.min + ImVec2(std::max(kMinWidth, textWidth(node.type) + 2 * kPad),
                         kHeader + kRow);
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
      extra = kSwatchWidth;
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

  const float width =
      std::max({kMinWidth,
                textWidth(type->label) + 2 * kPad,
                inputsWidth + outputsWidth + 3 * kPad + 2 * kPinRadius,
                propertyWidth + 2 * kPad});
  const int propertyRows = type->property == mg::Property::None ? 0 : 1;
  const std::size_t rows = std::max(type->inputs.size(), type->outputs.size());
  out.max = out.min +
            ImVec2(width,
                   kHeader + (propertyRows + static_cast<float>(rows)) * kRow +
                       kPad * 0.5f);

  const float top = out.min.y + kHeader + propertyRows * kRow + kRow * 0.5f;
  for (std::size_t i = 0; i < type->inputs.size(); ++i)
    out.inputs.push_back({out.min.x, top + i * kRow});
  for (std::size_t i = 0; i < type->outputs.size(); ++i)
    out.outputs.push_back({out.max.x, top + i * kRow});
  return out;
}

ImVec2 MaterialGraphCanvas::toScreen(ImVec2 canvas) const
{
  return m_origin + m_pan + canvas * m_zoom;
}

ImVec2 MaterialGraphCanvas::toCanvas(ImVec2 screen) const
{
  return (screen - m_origin - m_pan) * (1.0f / m_zoom);
}

std::optional<MaterialGraphCanvas::Pin>
MaterialGraphCanvas::pinAt(ImVec2 screen) const
{
  const float reach = std::max(kPinRadius * 2.0f * m_zoom, 7.0f);
  for (auto it = m_pins.rbegin(); it != m_pins.rend(); ++it)
    if (ImLengthSqr(toScreen(it->second) - screen) <= reach * reach)
      return it->first;
  return std::nullopt;
}

MaterialGraphCanvas::NodeId MaterialGraphCanvas::nodeAt(ImVec2 screen) const
{
  const ImVec2 point = toCanvas(screen);
  for (auto it = m_layouts.rbegin(); it != m_layouts.rend(); ++it)
    if (ImRect(it->second.min, it->second.max).Contains(point))
      return it->first;
  return 0;
}

void MaterialGraphCanvas::fit(ImVec2 size)
{
  ImRect bounds(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
  bool any = false;
  for (const auto& [id, nodeLayout] : m_layouts)
    if (m_selection.empty() || m_selection.contains(id))
    {
      bounds.Add(ImRect(nodeLayout.min, nodeLayout.max));
      any = true;
    }
  if (!any || size.x <= 0.0f || size.y <= 0.0f)
    return;
  const float margin = 60.0f;
  const ImVec2 extent = bounds.GetSize() + ImVec2(2 * margin, 2 * margin);
  m_zoom = std::clamp(
      std::min(size.x / extent.x, size.y / extent.y), kMinZoom, 1.25f);
  m_pan = size * 0.5f - bounds.GetCenter() * m_zoom;
}

void MaterialGraphCanvas::layoutNodes(const Graph& graph,
                                      const mg::CompiledGraph& compiled,
                                      const sfs::AssetDatabase* assets)
{
  // Selected nodes draw (and are hit) last: on top.
  m_layouts.clear();
  m_pins.clear();
  for (int pass = 0; pass < 2; ++pass)
    for (const mg::Node& node : graph.nodes)
      if (m_selection.contains(node.id) == (pass == 1))
      {
        Layout nodeLayout = layout(node, compiled, assets);
        for (uint32_t i = 0; i < nodeLayout.inputs.size(); ++i)
          m_pins.push_back({{node.id, i, false}, nodeLayout.inputs[i]});
        for (uint32_t i = 0; i < nodeLayout.outputs.size(); ++i)
          m_pins.push_back({{node.id, i, true}, nodeLayout.outputs[i]});
        m_layouts.emplace_back(node.id, std::move(nodeLayout));
      }
}

ImVec2 MaterialGraphCanvas::placement() const
{
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const ImRect view(m_origin, m_origin + m_size);
  return toCanvas(view.Contains(mouse) ? mouse : view.GetCenter());
}

// --- drawing
// -------------------------------------------------------------------

void MaterialGraphCanvas::draw(Graph& graph,
                               const mg::CompiledGraph& compiled,
                               const sfs::AssetDatabase* assets)
{
  m_origin = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  const bool settled = size.x == m_size.x && size.y == m_size.y;
  m_size = size;
  if (size.x < 2.0f || size.y < 2.0f)
    return;

  // Laid out for the mouse's hit tests, then again for drawing what it did
  // (so moved nodes don't lag a frame behind).
  layoutNodes(graph, compiled, assets);
  // Framed once the canvas has its size: a tab just docked (or splitting its
  // space) takes a frame or two to settle.
  if (m_frameRequested && settled && size.x > 64.0f && size.y > 64.0f)
  {
    m_frameRequested = false;
    fit(size);
  }

  // The nodes' fields, drawn after, take the mouse where they are.
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("canvas",
                         size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  acceptTextureDrop(graph, assets);
  handleMouse(graph);
  layoutNodes(graph, compiled, assets);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->PushClipRect(m_origin, m_origin + size, true);
  draw->AddRectFilled(m_origin, m_origin + size, IM_COL32(34, 36, 35, 255));
  drawGrid(draw, size);
  drawLinks(draw, graph, compiled);
  for (const auto& [id, nodeLayout] : m_layouts)
    if (mg::Node* node = graph.find(id))
    {
      drawNode(draw, *node, graph, compiled, assets);
      // Right after its node, so a node on top covers the one below's.
      if (fieldsShown())
        drawFields(*node, graph, compiled);
    }

  const ImVec2 mouse = ImGui::GetIO().MousePos;
  if (m_gesture == Gesture::Link)
  {
    // The link being dragged, from its pin to the mouse.
    const auto it =
        std::find_if(m_pins.begin(),
                     m_pins.end(),
                     [&](const auto& p) { return p.first == m_linkFrom; });
    if (it != m_pins.end())
    {
      ImVec2 a = toScreen(it->second), b = mouse;
      if (!m_linkFrom.output)
        std::swap(a, b);
      const float dx = std::max(std::abs(b.x - a.x) * 0.5f, 40.0f * m_zoom);
      draw->AddBezierCubic(a,
                           a + ImVec2(dx, 0),
                           b - ImVec2(dx, 0),
                           b,
                           theme::kAccent,
                           std::max(2.0f * m_zoom, 1.5f));
    }
  }
  if (m_gesture == Gesture::BoxSelect)
  {
    draw->AddRectFilled(m_pressedAt, mouse, theme::kAccentSoft);
    draw->AddRect(m_pressedAt, mouse, theme::kAccent);
  }
  draw->PopClipRect();

  drawAddMenu(graph);
  drawNodeMenu(graph);
  drawColorPicker(graph);
}

bool MaterialGraphCanvas::fieldsShown() const { return m_zoom >= kFieldsZoom; }

void MaterialGraphCanvas::drawFields(mg::Node& node,
                                     const Graph& graph,
                                     const mg::CompiledGraph& compiled)
{
  const mg::NodeType* type = mg::findNodeType(node.type);
  const auto it =
      std::find_if(m_layouts.begin(),
                   m_layouts.end(),
                   [&](const auto& l) { return l.first == node.id; });
  if (!type || it == m_layouts.end())
    return;
  const Layout& nodeLayout = it->second;
  const mg::ValueType dynamic = dynamicOf(compiled, node.id);

  // Widgets at the canvas's zoom: the font and spacing scaled with it, frames
  // a row high.
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::PushFont(nullptr, style.FontSizeBase * m_zoom);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, 1.0f) * m_zoom);
  ImGui::PushStyleVar(
      ImGuiStyleVar_ItemInnerSpacing, ImVec2(2.0f, 2.0f) * m_zoom);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * m_zoom);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 6.0f * m_zoom);
  ImGui::PushID(static_cast<int>(node.id));
  const float frameHeight = ImGui::GetFrameHeight();
  const ImVec2 min = toScreen(nodeLayout.min), max = toScreen(nodeLayout.max);

  // Editing a node's value selects it.
  const auto selectIfEdited = [&]
  {
    if (ImGui::IsItemActivated())
      m_selection = {node.id};
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
      m_selection = {node.id};
    }
  };

  // The property row: a Colour's colour, a Number's number.
  const float propertyTop = min.y + kHeader * m_zoom;
  const float rowWidth = max.x - min.x - 2.0f * kPad * m_zoom;
  if (type->property == mg::Property::Color)
  {
    ImGui::SetCursorScreenPos(
        ImVec2(min.x + kPad * m_zoom,
               propertyTop + (kRow * m_zoom - frameHeight) * 0.5f));
    colorField("##value", node.value, -1, ImVec2(rowWidth, frameHeight));
  }
  else if (type->property == mg::Property::Number)
  {
    ImGui::SetCursorScreenPos(
        ImVec2(min.x + kPad * m_zoom,
               propertyTop + (kRow * m_zoom - frameHeight) * 0.5f));
    ImGui::SetNextItemWidth(rowWidth);
    ImGui::DragFloat("##value", &node.value.x, 0.01f, 0.0f, 0.0f, "%.3g");
    selectIfEdited();
  }

  // Each unlinked input's own value, after its name.
  for (uint32_t i = 0; i < type->inputs.size() && i < node.inputs.size(); ++i)
  {
    const mg::InputPin& pin = type->inputs[i];
    if (pin.fallback != mg::Fallback::Value || graph.linkInto(node.id, i))
      continue;
    ImGui::PushID(static_cast<int>(i));
    const float nameWidth = ImGui::CalcTextSize(pin.name.c_str()).x;
    const ImVec2 centre = toScreen(nodeLayout.inputs[i]);
    ImGui::SetCursorScreenPos(
        ImVec2(min.x + (kPad + kPinRadius + 8.0f) * m_zoom + nameWidth,
               centre.y - frameHeight * 0.5f));
    glm::vec4& value = node.inputs[i];
    const mg::ValueType inputType = type->inputType(i, dynamic);
    if (pin.color)
      colorField("##value",
                 value,
                 static_cast<int>(i),
                 ImVec2(kSwatchWidth * m_zoom, frameHeight));
    else
    {
      ImGui::SetNextItemWidth(fieldWidth(inputType) * m_zoom);
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

void MaterialGraphCanvas::drawGrid(ImDrawList* draw, ImVec2 size) const
{
  const float step = kGrid * m_zoom;
  if (step < 6.0f)
    return;
  const ImVec2 offset(std::fmod(m_pan.x, step), std::fmod(m_pan.y, step));
  for (float x = offset.x; x < size.x; x += step)
    draw->AddLine(m_origin + ImVec2(x, 0),
                  m_origin + ImVec2(x, size.y),
                  IM_COL32(255, 255, 255, 10));
  for (float y = offset.y; y < size.y; y += step)
    draw->AddLine(m_origin + ImVec2(0, y),
                  m_origin + ImVec2(size.x, y),
                  IM_COL32(255, 255, 255, 10));
}

void MaterialGraphCanvas::drawLinks(ImDrawList* draw,
                                    const Graph& graph,
                                    const mg::CompiledGraph& compiled) const
{
  const auto pinPosition = [&](const Pin& pin) -> std::optional<ImVec2>
  {
    for (const auto& [p, position] : m_pins)
      if (p == pin)
        return toScreen(position);
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
    const float dx = std::max(std::abs(to->x - from->x) * 0.5f, 40.0f * m_zoom);
    draw->AddBezierCubic(*from,
                         *from + ImVec2(dx, 0),
                         *to - ImVec2(dx, 0),
                         *to,
                         color,
                         std::max(2.0f * m_zoom, 1.2f));
  }
}

void MaterialGraphCanvas::drawNode(ImDrawList* draw,
                                   const mg::Node& node,
                                   const Graph& graph,
                                   const mg::CompiledGraph& compiled,
                                   const sfs::AssetDatabase* assets) const
{
  const auto it =
      std::find_if(m_layouts.begin(),
                   m_layouts.end(),
                   [&](const auto& l) { return l.first == node.id; });
  if (it == m_layouts.end())
    return;
  const Layout& nodeLayout = it->second;
  const mg::NodeType* type = mg::findNodeType(node.type);
  const ImVec2 min = toScreen(nodeLayout.min), max = toScreen(nodeLayout.max);
  const float rounding = 5.0f * m_zoom;
  const float fontSize = ImGui::GetFontSize() * m_zoom;
  const bool showText = fontSize >= 5.0f;
  ImFont* font = ImGui::GetFont();
  const auto text = [&](ImVec2 at, ImU32 color, const std::string& value)
  {
    if (showText)
      draw->AddText(font, fontSize, at, color, value.c_str());
  };
  const float textOffset = (kRow * m_zoom - fontSize) * 0.5f;

  // Body, header and outline.
  draw->AddRectFilled(min, max, theme::kTile, rounding);
  const ImVec2 headerMax(max.x, min.y + kHeader * m_zoom);
  draw->AddRectFilled(
      min,
      headerMax,
      type ? categoryColor(type->category) : IM_COL32(110, 60, 60, 255),
      rounding,
      ImDrawFlags_RoundCornersTop);
  text(min + ImVec2(kPad * m_zoom, (kHeader * m_zoom - fontSize) * 0.5f),
       IM_COL32(236, 238, 232, 255),
       type ? type->label : node.type + " (unknown)");

  const mg::CompileError* error = errorOf(compiled, node.id);
  const bool selected = m_selection.contains(node.id);
  const ImU32 outline = error      ? kErrorColor
                        : selected ? theme::kAccent
                                   : IM_COL32(20, 22, 21, 255);
  draw->AddRect(min,
                max,
                outline,
                rounding,
                0,
                (error || selected) ? std::max(2.0f * m_zoom, 1.5f) : 1.0f);
  if (error && ImGui::IsItemHovered() &&
      ImRect(min, max).Contains(ImGui::GetIO().MousePos) &&
      m_gesture == Gesture::None)
    ImGui::SetTooltip("%s", error->message.c_str());
  if (!type)
    return;

  // The property row: the node's colour, number or image.
  const mg::ValueType dynamic = dynamicOf(compiled, node.id);
  float y = min.y + kHeader * m_zoom;
  if (type->property != mg::Property::None)
  {
    const ImVec2 rowMin(min.x + kPad * m_zoom, y + 3.0f * m_zoom);
    const ImVec2 rowMax(max.x - kPad * m_zoom, y + (kRow - 3.0f) * m_zoom);
    // A Colour's colour and a Number's number are fields while those show.
    if (type->property == mg::Property::Color && !fieldsShown())
    {
      draw->AddRectFilled(rowMin,
                          rowMax,
                          ImGui::ColorConvertFloat4ToU32(ImVec4(
                              node.value.r, node.value.g, node.value.b, 1.0f)),
                          3.0f * m_zoom);
      draw->AddRect(rowMin, rowMax, IM_COL32(0, 0, 0, 120), 3.0f * m_zoom);
    }
    else if (type->property == mg::Property::Number && !fieldsShown())
      text(ImVec2(rowMin.x, y + textOffset),
           IM_COL32(220, 222, 216, 255),
           valueText(node.value, mg::ValueType::Float));
    else if (type->property == mg::Property::Texture)
      text(ImVec2(rowMin.x, y + textOffset),
           IM_COL32(220, 222, 216, 255),
           textureLabel(node, assets) + (node.dataTexture ? " (data)" : ""));
    y += kRow * m_zoom;
  }

  // Inputs on the left, each with its own value while unlinked; outputs on
  // the right.
  for (uint32_t i = 0; i < type->inputs.size(); ++i)
  {
    const mg::InputPin& pin = type->inputs[i];
    const ImVec2 centre = toScreen(nodeLayout.inputs[i]);
    const bool linked = graph.linkInto(node.id, i) != nullptr;
    const ImU32 color = typeColor(type->inputType(i, dynamic));
    if (linked)
      draw->AddCircleFilled(centre, kPinRadius * m_zoom, color);
    else
      draw->AddCircle(
          centre, kPinRadius * m_zoom, color, 0, std::max(1.5f * m_zoom, 1.0f));
    const ImVec2 at(
        min.x + (kPad + kPinRadius) * m_zoom, centre.y - fontSize * 0.5f);
    text(at, IM_COL32(214, 216, 210, 255), pin.name);
    if (linked)
      continue;
    const float nameWidth =
        showText
            ? font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, pin.name.c_str()).x
            : 0.0f;
    const ImVec2 valueAt = at + ImVec2(nameWidth + 8.0f * m_zoom, 0.0f);
    if (pin.fallback != mg::Fallback::Value)
      text(valueAt,
           IM_COL32(150, 154, 148, 255),
           mg::fallbackName(pin.fallback));
    else if (fieldsShown())
      continue; // its field shows the value
    else if (i < node.inputs.size() && pin.color)
    {
      const ImVec2 swatch(12.0f * m_zoom, 12.0f * m_zoom);
      const ImVec2 swatchMin =
          valueAt + ImVec2(0.0f, (fontSize - swatch.y) * 0.5f);
      const glm::vec4& v = node.inputs[i];
      draw->AddRectFilled(
          swatchMin,
          swatchMin + swatch,
          ImGui::ColorConvertFloat4ToU32(ImVec4(v.r, v.g, v.b, 1.0f)),
          2.0f * m_zoom);
    }
    else if (i < node.inputs.size())
      text(valueAt,
           IM_COL32(150, 154, 148, 255),
           valueText(node.inputs[i], type->inputType(i, dynamic)));
  }
  for (uint32_t i = 0; i < type->outputs.size(); ++i)
  {
    const ImVec2 centre = toScreen(nodeLayout.outputs[i]);
    const ImU32 color = typeColor(type->outputType(i, dynamic));
    const bool linked = std::any_of(
        graph.links.begin(),
        graph.links.end(),
        [&](const mg::Link& link)
        { return link.fromNode == node.id && link.fromOutput == i; });
    if (linked)
      draw->AddCircleFilled(centre, kPinRadius * m_zoom, color);
    else
      draw->AddCircle(
          centre, kPinRadius * m_zoom, color, 0, std::max(1.5f * m_zoom, 1.0f));
    if (showText)
    {
      const float width =
          font->CalcTextSizeA(
                  fontSize, FLT_MAX, 0.0f, type->outputs[i].name.c_str())
              .x;
      text(ImVec2(max.x - (kPad + kPinRadius) * m_zoom - width,
                  centre.y - fontSize * 0.5f),
           IM_COL32(214, 216, 210, 255),
           type->outputs[i].name);
    }
  }
}

// --- mouse -------------------------------------------------------------------

void MaterialGraphCanvas::handleMouse(Graph& graph)
{
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mouse = io.MousePos;
  const bool hovered = ImGui::IsItemHovered();

  // Zoom about the mouse.
  if (hovered && io.MouseWheel != 0.0f && m_gesture != Gesture::MoveNodes)
  {
    const ImVec2 under = toCanvas(mouse);
    m_zoom =
        std::clamp(m_zoom * std::pow(1.15f, io.MouseWheel), kMinZoom, kMaxZoom);
    m_pan = mouse - m_origin - under * m_zoom;
  }

  // Left button: links, moving, selecting.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      m_gesture == Gesture::None)
  {
    m_pressedAt = mouse;
    const bool adding = io.KeyCtrl || io.KeyShift;
    if (const std::optional<Pin> pin = pinAt(mouse))
    {
      if (io.KeyAlt)
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
      }
      else if (const mg::Link* link =
                   pin->output ? nullptr
                               : graph.linkInto(pin->node, pin->index))
      {
        // Pick the link up from this input, to put it somewhere else.
        m_linkFrom = {link->fromNode, link->fromOutput, true};
        m_linkPickedUp = true;
        graph.disconnect(pin->node, pin->index);
        m_gesture = Gesture::Link;
      }
      else
      {
        m_linkFrom = *pin;
        m_linkPickedUp = false;
        m_gesture = Gesture::Link;
      }
    }
    else if (const NodeId node = nodeAt(mouse))
    {
      if (adding)
      {
        if (!m_selection.erase(node))
          m_selection.insert(node);
      }
      else if (!m_selection.contains(node))
        m_selection = {node};
      if (m_selection.contains(node))
      {
        m_moveStart.clear();
        for (const NodeId id : m_selection)
          if (const mg::Node* selected = graph.find(id))
            m_moveStart.emplace_back(
                id, ImVec2(selected->position.x, selected->position.y));
        m_gesture = Gesture::MoveNodes;
      }
    }
    else
    {
      if (!adding)
        m_selection.clear();
      m_gesture = Gesture::BoxSelect;
    }
  }

  // Panning: the middle button, or dragging the right one.
  if (hovered && m_gesture == Gesture::None &&
      (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
       ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)))
    m_gesture = Gesture::Pan;

  switch (m_gesture)
  {
  case Gesture::Pan:
    m_pan = m_pan + io.MouseDelta;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right))
      m_gesture = Gesture::None;
    return; // a right-drag isn't a right-click
  case Gesture::MoveNodes:
  {
    const ImVec2 delta = (mouse - m_pressedAt) * (1.0f / m_zoom);
    for (const auto& [id, start] : m_moveStart)
      if (mg::Node* node = graph.find(id))
        node->position = {start.x + delta.x, start.y + delta.y};
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
      m_gesture = Gesture::None;
    break;
  }
  case Gesture::BoxSelect:
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      ImRect box(toCanvas(m_pressedAt), toCanvas(mouse));
      box = ImRect(ImMin(box.Min, box.Max), ImMax(box.Min, box.Max));
      for (const auto& [id, nodeLayout] : m_layouts)
        if (box.Overlaps(ImRect(nodeLayout.min, nodeLayout.max)))
          m_selection.insert(id);
      m_gesture = Gesture::None;
    }
    break;
  case Gesture::Link:
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      m_gesture = Gesture::None;
      finishLink(graph, mouse);
    }
    break;
  case Gesture::None:
    break;
  }

  // A right-click (not a drag): the node's menu, or Add Node.
  if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      m_gesture == Gesture::None &&
      !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right, 4.0f))
  {
    if (const NodeId node = nodeAt(mouse))
    {
      if (!m_selection.contains(node))
        m_selection = {node};
      m_menuNode = node;
      ImGui::OpenPopup("##nodeMenu");
    }
    else
    {
      m_addLink.reset();
      m_openAddMenu = true;
    }
  }
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
    const ImVec2 at = toCanvas(ImGui::GetIO().MousePos);
    const NodeId node = graph.add("texture", {at.x, at.y});
    graph.find(node)->texture = "asset:" + dragged->id.toString();
    m_selection = {node};
  }
  ImGui::EndDragDropTarget();
}

// --- menus -------------------------------------------------------------------

void MaterialGraphCanvas::addNode(Graph& graph, const mg::NodeType& type)
{
  const NodeId node = graph.add(type.id, {m_addAt.x, m_addAt.y});
  if (!node)
    return;
  m_selection = {node};
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
  for (const NodeId id : m_selection)
    if (const mg::Node* node = graph.find(id);
        node && node->type != mg::kOutputType)
      graph.remove(id);
  std::erase_if(m_selection, [&](NodeId id) { return !graph.find(id); });
}

namespace
{
// The selected nodes and the links between them, as JSON; with
// `withOutput`, the Material Output too if it's selected (copying a whole
// graph, to paste into another).
std::string selectionJson(const mg::Graph& graph,
                          const std::set<mg::NodeId>& selection,
                          bool withOutput)
{
  mg::Graph copy;
  for (const mg::Node& node : graph.nodes)
    if (selection.contains(node.id) &&
        (withOutput || node.type != mg::kOutputType))
      copy.nodes.push_back(node);
  for (const mg::Link& link : graph.links)
    if (copy.find(link.fromNode) && copy.find(link.toNode))
      copy.links.push_back(link);
  if (copy.nodes.empty())
    return {};
  return nlohmann::json{{kClipboardKey, copy.toJson()}}.dump();
}
} // namespace

void MaterialGraphCanvas::copySelection(const Graph& graph) const
{
  const std::string json = selectionJson(graph, m_selection, true);
  if (!json.empty())
    ImGui::SetClipboardText(json.c_str());
}

void MaterialGraphCanvas::paste(Graph& graph)
{
  const char* text = ImGui::GetClipboardText();
  if (!text)
    return;
  pasteJson(graph, text, placement());
}

void MaterialGraphCanvas::duplicateSelection(Graph& graph)
{
  ImVec2 topLeft(FLT_MAX, FLT_MAX);
  for (const NodeId id : m_selection)
    if (const mg::Node* node = graph.find(id);
        node && node->type != mg::kOutputType)
      topLeft = ImMin(topLeft, ImVec2(node->position.x, node->position.y));
  const std::string json = selectionJson(graph, m_selection, false);
  if (!json.empty())
    pasteJson(graph, json, topLeft + ImVec2(40.0f, 40.0f));
}

void MaterialGraphCanvas::pasteJson(Graph& graph,
                                    const std::string& text,
                                    ImVec2 at)
{
  const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
  if (!json.is_object() || !json.contains(kClipboardKey))
    return;
  const std::optional<Graph> pasted = Graph::fromJson(json[kClipboardKey]);
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
    for (const auto& [id, nodeLayout] : m_layouts)
      if (id != output->id)
        ownBounds.Add(ImRect(nodeLayout.min, nodeLayout.max));
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
  m_selection.clear();
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
    m_selection.insert(node.id);
    graph.nodes.push_back(std::move(node));
  }
  for (const mg::Link& link : pasted->links)
    if (ids.contains(link.fromNode) && ids.contains(link.toNode))
      graph.connect(
          ids[link.fromNode], link.fromOutput, ids[link.toNode], link.toInput);
}

void MaterialGraphCanvas::selectAll(const Graph& graph)
{
  m_selection.clear();
  for (const mg::Node& node : graph.nodes)
    m_selection.insert(node.id);
}
