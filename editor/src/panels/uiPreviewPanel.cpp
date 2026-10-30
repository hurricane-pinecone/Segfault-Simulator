#include "panels/uiPreviewPanel.h"

#include "editorContext.h"
#include "panels/codePanel.h"
#include "scene/editorScene.h"
#include "widgets/theme.h"

#include "engine/core/components/scriptComponent.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/runtime/rendering/modules/uiModule.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "engine/runtime/ui/layer.h"
#include "engine/runtime/ui/preview.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace
{
// A layer's screen (in points), and the room round it, a share of it.
constexpr glm::vec2 kScreen(1920.0f, 1080.0f);
constexpr float kScreenRoom = 0.06f;
// Round previews (points).
constexpr float kRoom = 24.0f;
// The tallest (and widest) the previews are drawn, in pixels: WebGPU's
// default texture limit.
constexpr float kMaxTarget = 8192.0f;

// The preview's scene: apart from the editor's (whose ids start at 1).
constexpr sfs::SceneId kPreviewSceneId = 0xFFFF;

// What a file's part of: its folder and name, without extensions, so a
// view's .h, .ui.h and .cpp are one ("objects/FarmHud").
std::string unitOf(const std::filesystem::path& file)
{
  std::string name = file.filename().string();
  name = name.substr(0, name.find('.'));
  return file.parent_path().filename().string() + "/" + name;
}

std::string nameOf(const std::string& unit)
{
  return unit.substr(unit.find('/') + 1);
}

// The UI layer scripts the game's module has.
std::vector<std::string> layerNames()
{
  std::vector<std::string> names;
  for (const sfs::ScriptType& type : sfs::scripts().types())
    if (type.kind == sfs::ScriptType::Kind::GameObject && type.uiLayer)
      names.push_back(type.name);
  return names;
}

// The files with `previews`, as units, sorted by name.
std::vector<std::string>
shownUnits(const std::vector<const sfs::ui::Preview*>& previews)
{
  std::set<std::string> units;
  for (const sfs::ui::Preview* preview : previews)
    units.insert(unitOf(preview->file()));
  std::vector<std::string> sorted(units.begin(), units.end());
  std::sort(sorted.begin(),
            sorted.end(),
            [](const std::string& a, const std::string& b)
            { return nameOf(a) < nameOf(b); });
  return sorted;
}

// Lays out a View for the UI module, which lays out layers.
class ViewLayer : public sfs::ui::Layer
{
public:
  sfs::ui::View body() override { return view; }
  sfs::ui::View view;
};

ImU32 colour(int r, int g, int b, float a = 1.0f)
{
  return IM_COL32(r, g, b, static_cast<int>(a * 255.0f));
}
// What `ours` has, and those of `all` from files `ours` hasn't any from.
template <typename T>
std::vector<const T*> withOthers(const std::vector<const T*>& ours,
                                 const std::vector<const T*>& all)
{
  std::set<std::string> units;
  for (const T* item : ours)
    units.insert(unitOf(item->file()));
  std::vector<const T*> merged = ours;
  for (const T* item : all)
    if (std::ranges::find(ours, item) == ours.end() &&
        !units.contains(unitOf(item->file())))
      merged.push_back(item);
  return merged;
}

} // namespace

UiPreviewPanel::UiPreviewPanel(sfs::IRenderBackend& backend,
                               const sfs::SceneServices& services,
                               const CodePanel& code)
    : Panel("UI Preview"), m_backend(backend), m_services(services),
      m_code(code)
{
}

UiPreviewPanel::~UiPreviewPanel()
{
  dropObjects();
  dropPreviewModule();
}

void UiPreviewPanel::dropPreviewModule()
{
  // Nothing made by it may outlive it: the views laid out last frame hold
  // its code (bodies, actions).
  m_ui.reset();
  m_modulePreviews.clear();
  m_moduleStyles.clear();
  m_previewModule.unload();
}

void UiPreviewPanel::loadPreviewModule(const std::filesystem::path& module)
{
  dropPreviewModule();
  m_previewModule.setQuiet(true);
  // Its previews (and styles) are the ones made as it loads.
  const std::vector<const sfs::ui::Preview*> before = sfs::ui::previews();
  const std::vector<const sfs::ui::PreviewStyle*> stylesBefore =
      sfs::ui::previewStyles();
  if (!m_previewModule.load(
          module, module.stem().string(), sfs::ModuleHost::Load::FromCopy))
    return;
  for (const sfs::ui::Preview* preview : sfs::ui::previews())
    if (std::ranges::find(before, preview) == before.end())
      m_modulePreviews.push_back(preview);
  for (const sfs::ui::PreviewStyle* style : sfs::ui::previewStyles())
    if (std::ranges::find(stylesBefore, style) == stylesBefore.end())
      m_moduleStyles.push_back(style);
  ++m_previewLoads;
}

std::vector<const sfs::ui::Preview*> UiPreviewPanel::allPreviews() const
{
  return withOthers(m_modulePreviews, sfs::ui::previews());
}

void UiPreviewPanel::dropObjects()
{
  // The module before the scene: it lays the copy out.
  m_copy = nullptr;
  m_copyOf.clear();
  m_ui.reset();
  m_scene.reset();
}

void UiPreviewPanel::beforeBegin()
{
  // A new UI Preview joins the Console's dock node (under the viewport, so
  // it shows beside the Code tab).
  if (!m_dockChecked)
  {
    m_dockChecked = true;
    const ImGuiWindow* console = ImGui::FindWindowByName("Console");
    if (console && console->DockId &&
        !ImGui::FindWindowSettingsByID(ImHashStr(title())))
      ImGui::SetNextWindowDockID(console->DockId);
  }
  // A UI file opened: its tab to the front, without taking the keyboard
  // from the Code panel.
  if (std::exchange(m_bringForward, false))
    if (ImGuiWindow* window = ImGui::FindWindowByName(title()))
      if (window->DockNode && window->DockNode->TabBar)
        window->DockNode->TabBar->NextSelectedTabId = window->TabId;
}

void UiPreviewPanel::update(EditorContext&)
{
  // Every frame, shown or not: a UI file opened brings the tab forward.
  if (m_follow && open())
    followCode();
}

void UiPreviewPanel::followCode()
{
  const std::filesystem::path& file = m_code.focusedScript();
  // Checked again when the module changes (a file may be opened before the
  // game's built, or its first preview written).
  const std::size_t code = sfs::scripts().types().size() * 1000 +
                           allPreviews().size() + m_previewLoads * 1000000;
  if (file.empty() || (file == m_followed && code == m_codeSeen))
    return;
  const bool newFile = file != m_followed;
  m_followed = file;
  m_codeSeen = code;

  const std::string unit = unitOf(file);
  const std::vector<std::string> layers = layerNames();
  const bool previewed =
      std::ranges::any_of(allPreviews(),
                          [&](const sfs::ui::Preview* preview)
                          { return unitOf(preview->file()) == unit; });
  const bool isLayer = std::ranges::find(layers, nameOf(unit)) != layers.end();
  if (!previewed && !isLayer)
    return; // not UI: keep showing what was
  if (newFile || unit != m_unit)
    m_bringForward = true;
  m_unit = unit;
  m_layerName = isLayer ? nameOf(unit) : std::string();
}

std::vector<const sfs::ui::Preview*> UiPreviewPanel::shownPreviews() const
{
  std::vector<const sfs::ui::Preview*> shown;
  for (const sfs::ui::Preview* preview : allPreviews())
    if (unitOf(preview->file()) == m_unit)
      shown.push_back(preview);
  // The views (made in the .ui.h's order) before the declared previews.
  std::ranges::stable_partition(shown,
                                [](const sfs::ui::Preview* preview)
                                { return preview->file().ends_with(".ui.h"); });
  return shown;
}

sfs::ui::View UiPreviewPanel::styled(sfs::ui::View view) const
{
  const std::vector<const sfs::ui::PreviewStyle*> styles =
      withOthers(m_moduleStyles, sfs::ui::previewStyles());
  for (const sfs::ui::PreviewStyle* style : styles)
    if (unitOf(style->file()) == m_unit)
      view = style->apply(std::move(view));
  return view;
}

sfs::ui::Layer* UiPreviewPanel::layer(EditorContext& context)
{
  if (m_layerName.empty() || !context.scene)
    return nullptr;

  // Playing: the game's own.
  if (context.playing)
    for (sfs::ui::Layer* layer : context.scene->findObjects<sfs::ui::Layer>())
    {
      const sfs::Entity& entity = layer->entity();
      if (entity.isValid() && entity.hasComponent<sfs::ScriptComponent>() &&
          entity.getComponent<sfs::ScriptComponent>().script == m_layerName)
        return layer;
    }

  // Otherwise a copy, on the preview's own scene.
  if (m_copy && m_copyOf == m_layerName)
    return dynamic_cast<sfs::ui::Layer*>(m_copy);
  dropObjects();
  m_copyOf = m_layerName;
  const sfs::ScriptType* type = sfs::scripts().find(m_layerName);
  if (!type || !type->createObject)
    return nullptr;
  m_scene =
      std::make_unique<sfs::Scene>(kPreviewSceneId, m_services, "UI Preview");
  std::unique_ptr<sfs::GameObject> object = type->createObject();
  // The values an entity in the scene gives it, if one has it.
  if (const sfs::reflect::TypeInfo* info =
          sfs::reflect::types().find(m_layerName))
  {
    const sfs::SceneSerializer serializer(
        sfs::reflect::types(),
        context.render ? &context.render->meshes() : nullptr);
    for (const sfs::Entity& entity : context.scene->entities())
      if (entity.hasComponent<sfs::ScriptComponent>() &&
          entity.getComponent<sfs::ScriptComponent>().script == m_layerName)
      {
        serializer.loadObject(
            *info,
            object.get(),
            entity.getComponent<sfs::ScriptComponent>().fields);
        break;
      }
  }
  try
  {
    m_copy = &m_scene->adoptObject(std::move(object), m_scene->createEntity());
  }
  catch (const std::exception& error)
  {
    m_error = std::string("onCreate threw: ") + error.what();
    return nullptr;
  }
  return dynamic_cast<sfs::ui::Layer*>(m_copy);
}

void UiPreviewPanel::drawToolbar()
{
  std::vector<std::string> units = shownUnits(allPreviews());
  for (const std::string& name : layerNames())
    if (std::ranges::none_of(units,
                             [&](const std::string& unit)
                             { return nameOf(unit) == name; }))
      units.push_back("/" +
                      name); // a layer with no previews: its folder isn't known
  ImGui::SetNextItemWidth(200.0f);
  if (ImGui::BeginCombo(
          "##shown", m_unit.empty() ? "Nothing" : nameOf(m_unit).c_str()))
  {
    for (const std::string& unit : units)
      if (ImGui::Selectable(
              nameOf(unit).c_str(), nameOf(unit) == nameOf(m_unit)))
      {
        m_unit = unit;
        const std::vector<std::string> layers = layerNames();
        m_layerName = std::ranges::find(layers, nameOf(unit)) != layers.end()
                          ? nameOf(unit)
                          : std::string();
        m_follow = false; // picked: stay on it
      }
    if (units.empty())
      ImGui::TextDisabled("No previews or UI layers (build the game first)");
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("The previews (or UI layer) shown");
  ImGui::SameLine();
  ImGui::Checkbox("Follow Code", &m_follow);
  ImGui::SetItemTooltip("Show the previews of the file open in the Code panel");
  ImGui::SameLine();
  ImGui::Checkbox("Outlines", &m_outlines);
  ImGui::SetItemTooltip(
      "Outline the view under the mouse, with its size in points");
}

void UiPreviewPanel::drawContents(EditorContext& context)
{
  drawToolbar();
  if (m_buildFailed)
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                       "%s",
                       m_previewModule.loaded()
                           ? "The UI files don't build (see the Console): "
                             "showing them as they last did"
                           : "The UI files don't build (see the Console)");

  const float dpi = std::max(ImGui::GetWindowDpiScale(), 1.0f);
  const std::vector<const sfs::ui::Preview*> previews = shownPreviews();
  if (!previews.empty())
  {
    // Each preview under its name, as big as its view.
    // Named in the editor's font, not the game's.
    const sfs::ui::Font captionFont =
        theme::uiFontFile().empty()
            ? sfs::ui::Font::caption()
            : sfs::ui::Font::named(theme::uiFontFile(), 14.0f);
    // Each sized by itself, so one that fills what it's given (a ZStack, a
    // screen's worth) doesn't make the others fill the panel too: it gets
    // the panel's width (a small screen's, at least: 960 points) and a 16:9
    // screen's height.
    sfs::UiModule& ui = uiModule(context);
    ui.setPixelsPerPoint(dpi);
    const float screenWidth =
        std::max(ImGui::GetContentRegionAvail().x - 2.0f * kRoom, 960.0f);
    std::vector<sfs::ui::View> views;
    try
    {
      for (const sfs::ui::Preview* preview : previews)
      {
        sfs::ui::View view = styled(preview->make());
        const glm::vec2 size = ui.fit(view, glm::vec2(kMaxTarget)) / dpi;
        const bool wide = size.x * dpi >= kMaxTarget;
        const bool tall = size.y * dpi >= kMaxTarget;
        if (wide || tall)
        {
          const float width = wide ? screenWidth : size.x;
          view = view.frame(width, tall ? width * 9.0f / 16.0f : 0.0f);
        }
        views.push_back(sfs::ui::VStack(sfs::ui::Alignment::Leading,
                                        {
                                            sfs::ui::Text(preview->name())
                                                .font(captionFont)
                                                .color(sfs::ui::Color::rgb(
                                                    220, 220, 220, 0.6f)),
                                            view,
                                        })
                            .spacing(6.0f));
      }
    }
    catch (const std::exception& error)
    {
      m_error = std::string("A preview threw: ") + error.what();
      ImGui::TextColored(
          ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_error.c_str());
      return;
    }
    ViewLayer shown;
    shown.view = sfs::ui::VStack(std::move(views), sfs::ui::Alignment::Leading)
                     .spacing(kRoom);
    // Under the toolbar, scrolling if they're taller than the panel.
    ImGui::BeginChild("##previews",
                      ImVec2(0.0f, 0.0f),
                      0,
                      ImGuiWindowFlags_HorizontalScrollbar);
    drawUi(context, shown, glm::vec2(0.0f), dpi, kRoom * dpi, false);
    ImGui::EndChild();
    return;
  }

  sfs::ui::Layer* shown = layer(context);
  const bool live = shown && context.playing && shown != m_copy;
  if (m_unit.empty())
    ImGui::TextDisabled(
        "Open a UI file in the Code panel: its views show here");
  else if (!shown)
    ImGui::TextDisabled(
        "%s has no previews (declare a ui::Preview beside its views)",
        nameOf(m_unit).c_str());
  else if (live)
    ImGui::TextDisabled("The game's %s, live", m_layerName.c_str());
  else
    ImGui::TextDisabled("%s as it is outside Play. Declare a ui::Preview with "
                        "sample data to see it filled in",
                        m_layerName.c_str());
  if (!shown)
    return;

  // A layer is a screen: one fitted in, with room round it.
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float fit =
      std::min(avail.x / (kScreen.x * (1.0f + 2.0f * kScreenRoom)),
               avail.y / (kScreen.y * (1.0f + 2.0f * kScreenRoom)));
  const float userScale =
      context.render && context.render->module<sfs::UiModule>()
          ? context.render->module<sfs::UiModule>()->userScale()
          : 1.0f;
  if (fit * kScreen.y >= 40.0f)
    drawUi(context,
           *shown,
           glm::round(kScreen * fit * dpi),
           fit * dpi * userScale,
           kScreen.x * kScreenRoom * fit * dpi,
           true);
}

sfs::UiModule& UiPreviewPanel::uiModule(EditorContext& context)
{
  if (!m_ui)
  {
    if (!m_scene)
      m_scene = std::make_unique<sfs::Scene>(
          kPreviewSceneId, m_services, "UI Preview");
    m_ui = std::make_unique<sfs::UiModule>(*m_scene);
  }
  // The scene's look (a game sets its font there).
  if (context.render)
    if (sfs::UiModule* game = context.render->module<sfs::UiModule>())
      m_ui->theme() = game->theme();
  return *m_ui;
}

void UiPreviewPanel::drawUi(EditorContext& context,
                            sfs::ui::Layer& shown,
                            glm::vec2 screen,
                            float pixelsPerPoint,
                            float room,
                            bool screenGuides)
{
  if (m_failed)
    return;
  if (!m_initialized)
  {
    m_renderer = m_backend.createRenderer(256);
    m_target = m_backend.createRenderTarget();
    m_initialized = m_renderer && m_target;
    m_failed = !m_initialized;
    if (m_failed)
    {
      LOG_ERROR("The UI preview can't draw");
      return;
    }
  }
  uiModule(context);
  m_ui->setLayers([&shown] { return std::vector<sfs::ui::Layer*>{&shown}; });
  m_ui->setPixelsPerPoint(pixelsPerPoint);
  m_ui->setRecordPlaced(m_outlines);

  const float dpi = std::max(ImGui::GetWindowDpiScale(), 1.0f);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  try
  {
    // As big as it is, unless it's a screen: the panel scrolls. One that
    // stretches (maxWidth(infinity), a Spacer) takes the panel's width or
    // height that way.
    if (screen.x <= 0.0f)
    {
      const glm::vec2 panel = glm::max(
          glm::vec2(avail.x, avail.y) * dpi - 2.0f * room, glm::vec2(1.0f));
      const sfs::ui::View body = shown.body();
      screen = m_ui->fit(body, glm::vec2(kMaxTarget));
      if (screen.x >= kMaxTarget)
        screen = m_ui->fit(body, glm::vec2(panel.x, kMaxTarget));
      if (screen.y >= kMaxTarget)
        screen = m_ui->fit(body, glm::vec2(screen.x, panel.y));
      screen = glm::ceil(screen);
    }
    screen = glm::min(screen, glm::vec2(kMaxTarget - 2.0f * room));
  }
  catch (const std::exception& error)
  {
    m_error = std::string("body() threw: ") + error.what();
  }
  if (screen.x < 1.0f || screen.y < 1.0f)
    return;
  const glm::ivec2 targetPixels = glm::ivec2(screen + 2.0f * room);
  const glm::vec2 canvas = glm::vec2(targetPixels) / dpi; // points

  ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                       std::max(0.0f, (avail.x - canvas.x) * 0.5f));
  ImGui::InvisibleButton("##ui", ImVec2(canvas.x, canvas.y));
  const ImVec2 min = ImGui::GetItemRectMin();
  const bool hovered = ImGui::IsItemHovered();
  const ImGuiIO& io = ImGui::GetIO();
  const glm::vec2 mouse =
      glm::vec2(io.MousePos.x - min.x, io.MousePos.y - min.y) * dpi - room;
  const bool over = hovered && mouse.x >= 0.0f && mouse.y >= 0.0f &&
                    mouse.x < screen.x && mouse.y < screen.y;

  // Its buttons answer the mouse (a live layer's do what they do in game).
  m_ui->update(io.DeltaTime);
  m_ui->pointer(mouse,
                over,
                over && ImGui::IsMouseClicked(ImGuiMouseButton_Left),
                ImGui::IsMouseReleased(ImGuiMouseButton_Left),
                ImGui::IsMouseDown(ImGuiMouseButton_Left));
  sfs::RenderContext frameContext;
  frameContext.viewportSize = glm::ivec2(screen);
  frameContext.meshes = context.render ? &context.render->meshes() : nullptr;
  try
  {
    m_ui->computeCommands(frameContext);
    m_error.clear();
  }
  catch (const std::exception& error)
  {
    m_error = std::string("body() threw: ") + error.what();
  }
  if (!m_error.empty())
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_error.c_str());

  // Round it, the room; behind it, a plain backdrop (UI is drawn over a
  // game, not the panel's dark); then the UI.
  sfs::UiDrawList list;
  sfs::UiQuad back;
  back.rect = {0.0f, 0.0f, float(targetPixels.x), float(targetPixels.y)};
  back.color = {0.11f, 0.11f, 0.12f, 1.0f};
  list.quads.push_back(back);
  back.rect = {room, room, screen.x, screen.y};
  back.color = {0.36f, 0.42f, 0.36f, 1.0f};
  list.quads.push_back(back);
  for (const sfs::UiDrawCommand& command : m_ui->commands())
    if (command.list)
      for (sfs::UiQuad quad : command.list->quads)
      {
        quad.rect.x += room;
        quad.rect.y += room;
        list.quads.push_back(quad);
      }

  m_target->resize(targetPixels);
  if (!m_target->valid())
    return;
  sfs::FrameParams frame;
  frame.shadows.enabled = false;
  m_backend.drawInto(*m_target,
                     *m_renderer,
                     [&]
                     {
                       m_renderer->beginFrame(frame);
                       m_renderer->drawUi(list);
                       m_renderer->endFrame();
                     });

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddImage(static_cast<ImTextureID>(m_target->imguiTexture()),
                 min,
                 ImVec2(min.x + canvas.x, min.y + canvas.y));

  // Guides, in points: a screen's edge and middle.
  const ImVec2 s0(min.x + room / dpi, min.y + room / dpi);
  const ImVec2 s1(s0.x + screen.x / dpi, s0.y + screen.y / dpi);
  if (screenGuides)
  {
    draw->AddRect(s0, s1, colour(255, 255, 255, 0.35f));
    const float midX = (s0.x + s1.x) * 0.5f;
    const float midY = (s0.y + s1.y) * 0.5f;
    draw->AddLine(
        ImVec2(midX, s0.y), ImVec2(midX, s1.y), colour(255, 255, 255, 0.12f));
    draw->AddLine(
        ImVec2(s0.x, midY), ImVec2(s1.x, midY), colour(255, 255, 255, 0.12f));
  }
  if (!m_outlines || !over)
    return;

  // The innermost view under the mouse, named, with its size.
  const sfs::ui::Placed* under = nullptr;
  for (const sfs::ui::Placed& view : m_ui->placed())
    if (view.rect.contains(mouse) && view.kind != sfs::ui::Node::Kind::Empty &&
        (!under || view.depth >= under->depth))
      under = &view;
  if (!under)
    return;
  const ImVec2 a(s0.x + under->rect.x / dpi, s0.y + under->rect.y / dpi);
  const ImVec2 b(a.x + under->rect.width / dpi, a.y + under->rect.height / dpi);
  draw->AddRect(a, b, colour(255, 200, 80, 0.95f), 0.0f, 0, 1.5f);
  std::string label = sfs::ui::kindName(under->kind);
  if (!under->label.empty())
    label += " \"" + under->label.substr(0, 24) +
             (under->label.size() > 24 ? "...\"" : "\"");
  char size[48];
  std::snprintf(size,
                sizeof(size),
                "  %.0f x %.0f",
                under->rect.width / pixelsPerPoint,
                under->rect.height / pixelsPerPoint);
  label += size;
  const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
  ImVec2 at(a.x, a.y - textSize.y - 4.0f);
  if (at.y < min.y)
    at.y = b.y + 2.0f;
  at.x = std::clamp(
      at.x, min.x, std::max(min.x, min.x + canvas.x - textSize.x - 6.0f));
  draw->AddRectFilled(
      at,
      ImVec2(at.x + textSize.x + 6.0f, at.y + textSize.y + 2.0f),
      colour(30, 26, 20, 0.9f),
      3.0f);
  draw->AddText(
      ImVec2(at.x + 3.0f, at.y + 1.0f), colour(255, 220, 140), label.c_str());
}
