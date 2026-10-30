#include "panels/viewportPanel.h"

#include "assets/assetDrop.h"
#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "scene/prefabs.h"

#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/capsuleCollider3DComponent.h"
#include "engine/core/components/grassComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/pointLightComponent.h"
#include "engine/core/components/sphereCollider3DComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/rendering/grass.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/modules/grassModule.h"
#include "engine/runtime/rendering/picking.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "glm/glm/gtc/matrix_transform.hpp"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/gtc/type_ptr.hpp"

#include "imgui_internal.h" // FindWindowByName, a window's dock node
#include "imguizmo/ImGuizmo.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr float kMoveSnap = 0.5f;    // world units
constexpr float kRotateSnap = 15.0f; // degrees
constexpr float kScaleSnap = 0.1f;

// Whether an entity has grass the brush can paint: a GrassComponent on a mesh.
bool hasGrass(const sfs::Entity& entity)
{
  return entity.isValid() && entity.isAlive() &&
         entity.hasComponent<sfs::GrassComponent>() &&
         entity.hasComponent<sfs::MeshRendererComponent>();
}

constexpr ImU32 kSelectionColor = IM_COL32(255, 170, 40, 255);
// What a texture or material dragged over the viewport would land on.
constexpr ImU32 kDropColor = IM_COL32(90, 210, 255, 255);
constexpr ImU32 kLightColor = IM_COL32(255, 220, 120, 255);
constexpr ImU32 kCameraColor = IM_COL32(230, 230, 230, 255);
constexpr ImU32 kInactiveCameraColor = IM_COL32(140, 140, 140, 200);
constexpr ImU32 kColliderColor = IM_COL32(90, 230, 120, 255);
constexpr ImU32 kTriggerColor = IM_COL32(255, 150, 60, 255);

// How far ahead a camera's outline reaches, as a fraction of its distance from
// the viewer, so it stays about the same size on screen.
constexpr float kCameraIconScale = 0.08f;
} // namespace

ViewportPanel::ViewportPanel(sfs::IRenderBackend& backend, std::string title)
    : Panel(std::move(title)), m_backend(backend), m_target(backend.createRenderTarget())
{
}

ViewportPanel::~ViewportPanel()
{
  if (m_active)
    m_backend.setSceneTarget(nullptr);
}

void ViewportPanel::setActive(bool active)
{
  m_active = active;
  if (active && m_target)
    m_backend.setSceneTarget(m_target.get());
}

void ViewportPanel::beforeBegin()
{
  if (m_dockWith.empty())
    return;
  if (const ImGuiWindow* other = ImGui::FindWindowByName(m_dockWith.c_str()); other && other->DockId)
    ImGui::SetNextWindowDockID(other->DockId, ImGuiCond_Always);
  m_dockWith.clear();
}

int ViewportPanel::windowFlags() const
{
  // The image fills the panel; scrolling would only fight camera zoom. Keys
  // are for the viewport (shortcuts) and the game, not ImGui navigation.
  return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
         ImGuiWindowFlags_NoNavInputs | (m_unsaved ? ImGuiWindowFlags_UnsavedDocument : 0);
}

void ViewportPanel::update(EditorContext&)
{
  // Resize before the scene renders: the frame's ImGui draw (later this frame)
  // samples the target, so it must not be replaced after the panel is drawn.
  // An inactive viewport keeps its last frame (stretched if resized).
  if (m_active && m_target)
    m_target->resize(open() ? m_desiredSize : glm::ivec2(0, 0));
}

ViewportPanel::View ViewportPanel::currentView(const EditorContext& context,
                                               ImVec2 origin,
                                               ImVec2 size) const
{
  const sfs::Camera& camera = context.render->camera();
  View view;
  view.view = camera.viewMatrix();
  view.projection = camera.projectionMatrix(size.x / std::max(size.y, 1.0f));
  view.viewProjection = view.projection * view.view;
  view.origin = origin;
  view.size = size;
  return view;
}

std::optional<ImVec2> ViewportPanel::toScreen(const View& view, const glm::vec3& world) const
{
  const glm::vec4 clip = view.viewProjection * glm::vec4(world, 1.0f);
  if (clip.w <= 1.0e-4f)
    return std::nullopt; // behind the camera
  const glm::vec2 ndc = glm::vec2(clip) / clip.w;
  return ImVec2(view.origin.x + (ndc.x * 0.5f + 0.5f) * view.size.x,
                view.origin.y + (0.5f - ndc.y * 0.5f) * view.size.y);
}

void ViewportPanel::drawContents(EditorContext& context)
{
  const ImVec2 available = ImGui::GetContentRegionAvail();
  m_desiredSize = {
      static_cast<int>(available.x), static_cast<int>(available.y)};

  if (!m_active)
  {
    if (m_target && m_target->valid())
      ImGui::Image(static_cast<ImTextureID>(m_target->imguiTexture()), available);
    else
      ImGui::Dummy(available);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
      m_activationRequested = true;
    if (context.playing)
    {
      ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x + 12.0f, ImGui::GetItemRectMin().y + 12.0f));
      ImGui::TextUnformatted("Stop playing to edit here");
    }
    return;
  }

  if (!m_target || !m_target->valid() || m_target->size() != m_desiredSize || !context.render)
  {
    // First frame, or mid-resize: the correctly sized target arrives next
    // frame.
    ImGui::Dummy(available);
    return;
  }

  // A 2D game's view is its plane seen straight on; changing the scene's
  // style turns the view to match. (While playing, the game's camera rules.)
  const bool planar = context.render->space2D().has_value();
  if (!context.playing)
  {
    if (planar)
      ViewportCamera::makePlanar(context.render->camera());
    else if (m_wasPlanar)
      ViewportCamera::makeAngled(context.render->camera());
  }
  m_wasPlanar = planar;

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::Image(static_cast<ImTextureID>(m_target->imguiTexture()), available);
  const ImVec2 mouse = ImGui::GetMousePos();
  const bool overToolbar =
      mouse.x >= m_toolbarMin.x && mouse.y >= m_toolbarMin.y &&
      mouse.x <= m_toolbarMax.x && mouse.y <= m_toolbarMax.y;
  const bool imageHovered = ImGui::IsItemHovered() && !overToolbar;

  // Frame what a panel (or F) asked for. While playing the game's camera is
  // the one shown, so it waits.
  if (context.focusRequest.isValid() && !context.playing)
  {
    if (context.focusRequest.isAlive())
      if (const auto bounds = branchBounds(context, context.focusRequest))
        m_navigation.focus(context.render->camera(), *bounds);
    context.focusRequest = {};
  }
  const View view = currentView(context, origin, available);
  if (!context.playing)
    dropAsset(context, view); // targets the image, so straight after it

  // ImGui positions are on the desktop; the game wants them in the window.
  const ImGuiViewport* host = ImGui::GetWindowViewport();
  context.viewportWindowId = static_cast<uint32_t>(reinterpret_cast<intptr_t>(host->PlatformHandle));
  context.viewportRect = {static_cast<int>(origin.x - host->Pos.x),
                          static_cast<int>(origin.y - host->Pos.y),
                          static_cast<int>(available.x),
                          static_cast<int>(available.y)};

  // Editor overlays are hidden during Play unless asked for, so the viewport
  // looks like the game.
  bool gizmoActive = false;
  // A stroke ends with its tool (or when Play starts).
  if (m_tool != Tool::GrassBrush || context.playing || !gizmosVisible(context))
    endGrassStroke(context);
  if (gizmosVisible(context))
  {
    // Kept inside the image: an icon near its edge mustn't spill onto the
    // panel's frame.
    ImGui::PushClipRect(
        origin, ImVec2(origin.x + available.x, origin.y + available.y), true);
    drawLightIcons(context, view);
    drawCameraIcons(context, view);
    drawSelectionOutline(context, view);
    drawColliders(context, view);
    gizmoActive = m_tool == Tool::GrassBrush && !context.playing
                      ? paintGrass(context, view, imageHovered)
                      : drawGizmo(context, view);
    ImGui::PopClipRect();
  }

  if (context.playing)
  {
    // The game owns the camera and the keyboard while the viewport has focus
    // (like a game view); click into it to play.
    const bool focused = ImGui::IsWindowFocused();
    context.gameHasInput = focused;
    context.gameHasMouse = imageHovered;
    if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      ImGui::SetWindowFocus();
    const ImU32 border =
        focused ? IM_COL32(90, 200, 120, 255) : IM_COL32(200, 160, 60, 255);
    ImGui::GetWindowDrawList()->AddRect(
        origin,
        ImVec2(origin.x + available.x, origin.y + available.y),
        border,
        0.0f,
        0,
        2.0f);
    if (!focused)
    {
      ImGui::SetCursorScreenPos(
          ImVec2(origin.x + 8.0f, origin.y + available.y - 26.0f));
      ImGui::TextUnformatted("Click the viewport to control the game");
    }
  }
  else if (imageHovered)
  {
    handleShortcuts(context);
    if (!gizmoActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      pick(context, view);
    navigate(context, available.y);
  }

  // The toolbar floats over the scene on a soft dark backdrop, so it reads
  // over a bright sky as well as a dark night. The backdrop is drawn after
  // (once its size is known) but on a lower channel, so behind.
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->ChannelsSplit(2);
  drawList->ChannelsSetCurrent(1);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 12.0f, origin.y + 12.0f));
  ImGui::BeginGroup();
  drawToolbar(context);
  if (m_toolbarExtras)
  {
    ImGui::SameLine(0.0f, 18.0f);
    m_toolbarExtras(context);
  }
  ImGui::EndGroup();
  drawList->ChannelsSetCurrent(0);
  const ImVec2 barMin = ImGui::GetItemRectMin(),
               barMax = ImGui::GetItemRectMax();
  m_toolbarMin = ImVec2(barMin.x - 6.0f, barMin.y - 5.0f);
  m_toolbarMax = ImVec2(barMax.x + 8.0f, barMax.y + 5.0f);
  drawList->AddRectFilled(
      m_toolbarMin, m_toolbarMax, IM_COL32(38, 40, 39, 205), 8.0f);
  drawList->ChannelsMerge();
}

bool ViewportPanel::gizmosVisible(const EditorContext& context) const
{
  return context.playing ? m_gizmosInPlay : m_gizmosInEdit;
}

void ViewportPanel::drawToolbar(const EditorContext& context)
{
  // Separate settings for editing and Play: overlays usually help while
  // editing and get in the way of the game.
  ImGui::Checkbox(
      "Gizmos", context.playing ? &m_gizmosInPlay : &m_gizmosInEdit);
  ImGui::SetItemTooltip(
      "Show light icons, the selection outline and the gizmo\n(remembered "
      "separately for editing and Play)");
  // While editing, whether animated materials and grass's wind move; Play
  // always does.
  if (!context.playing)
  {
    ImGui::SameLine();
    ImGui::Checkbox("Animate", &m_animate);
    ImGui::SetItemTooltip(
        "Move animated materials and sway grass in the wind while editing\n"
        "(Play always does)");
  }
  if (context.render)
    context.render->setAnimating(context.playing || m_animate);
  if (!gizmosVisible(context))
    return;
  ImGui::SameLine();

  const auto toolButton = [&](const char* label, Tool tool, const char* tip)
  {
    const bool active = m_tool == tool;
    if (active)
      ImGui::PushStyleColor(
          ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label))
      m_tool = tool;
    if (active)
      ImGui::PopStyleColor();
    ImGui::SetItemTooltip("%s", tip);
    ImGui::SameLine();
  };

  toolButton("Move", Tool::Move, "Move (W)");
  toolButton("Rotate", Tool::Rotate, "Rotate (E)");
  toolButton("Scale", Tool::Scale, "Scale (R)");
  // The grass brush, for grass (only then).
  if (hasGrass(context.selection))
    toolButton("Grass",
               Tool::GrassBrush,
               "Paint where the grass grows (B): drag over it, Shift to erase");
  else if (m_tool == Tool::GrassBrush)
    m_tool = Tool::Move;
  if (m_tool == Tool::GrassBrush)
  {
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("Size",
                       &m_brushSize,
                       0.1f,
                       10.0f,
                       "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("How wide the brush is, in world units");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("Strength", &m_brushStrength, 0.05f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("How quickly it paints");
    ImGui::SameLine();
    // Which map: where it grows, how long, or data for its material.
    static constexpr const char* kChannels[] = {
        "Coverage", "Length", "Data 0", "Data 1", "Data 2", "Data 3"};
    ImGui::SetNextItemWidth(90.0f);
    ImGui::Combo(
        "##channel", &m_brushChannel, kChannels, IM_ARRAYSIZE(kChannels));
    ImGui::SetItemTooltip(
        "What it paints: where grass grows, how long it is, or data its "
        "material reads");
    ImGui::SameLine();
    ImGui::Checkbox("Erase", &m_brushErase);
    ImGui::SetItemTooltip("Take away instead (or hold Shift)");
    // All of it at once: grass everywhere, or none (to paint in from bare).
    const auto fillAll = [&](bool full)
    {
      auto* grassModule = context.render->module<sfs::GrassModule>();
      const sfs::GrassGround* ground =
          grassModule ? grassModule->ground(context.selection) : nullptr;
      if (!ground)
        return;
      auto& grass = context.selection.getComponent<sfs::GrassComponent>();
      nlohmann::json paint = nlohmann::json::object();
      if (!full)
      {
        sfs::GrassMaps bare(ground->grid());
        bare.fill(sfs::GrassMaps::Coverage, 0.0f);
        paint = bare.toJson();
      }
      context.history->change([&] { grass.paint = paint; });
    };
    ImGui::SameLine();
    if (ImGui::Button("Fill"))
      fillAll(true);
    ImGui::SetItemTooltip("Unpainted again: full, full-length grass all over");
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
      fillAll(false);
    ImGui::SetItemTooltip(
        "Clear all of it, to paint grass in where you want it");
    return;
  }
  if (ImGui::Button(m_localSpace ? "Local" : "World"))
    m_localSpace = !m_localSpace;
  ImGui::SetItemTooltip("Gizmo axes: world or the object's own");
  ImGui::SameLine();
  ImGui::Checkbox("Snap", &m_snap);
  ImGui::SetItemTooltip("Snap moves/rotations/scales (or hold Ctrl)");
}

void ViewportPanel::handleShortcuts(EditorContext& context)
{
  // Not while navigating (right-drag) or typing.
  if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::GetIO().WantTextInput)
    return;

  if (ImGui::IsKeyPressed(ImGuiKey_W, false))
    m_tool = Tool::Move;
  if (ImGui::IsKeyPressed(ImGuiKey_E, false))
    m_tool = Tool::Rotate;
  if (ImGui::IsKeyPressed(ImGuiKey_R, false))
    m_tool = Tool::Scale;
  if (ImGui::IsKeyPressed(ImGuiKey_B, false) && hasGrass(context.selection))
    m_tool = Tool::GrassBrush;

  if (ImGui::IsKeyPressed(ImGuiKey_F, false) && context.hasSelection())
    context.focusRequest = context.selection;
}

bool ViewportPanel::drawGizmo(EditorContext& context, const View& view)
{
  if (!context.hasSelection() || !context.selection.hasComponent<sfs::TransformComponent>())
    return false;

  ImGuizmo::SetDrawlist();
  ImGuizmo::SetRect(view.origin.x, view.origin.y, view.size.x, view.size.y);
  ImGuizmo::SetOrthographic(context.render->camera().projection ==
                            sfs::Camera::Projection::Orthographic);

  ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
  float snapValue = kMoveSnap;
  if (m_tool == Tool::Rotate)
  {
    operation = ImGuizmo::ROTATE;
    snapValue = kRotateSnap;
  }
  else if (m_tool == Tool::Scale)
  {
    operation = ImGuizmo::SCALE;
    snapValue = kScaleSnap;
  }
  const bool snapping = m_snap || ImGui::GetIO().KeyCtrl;
  const std::array<float, 3> snap{snapValue, snapValue, snapValue};
  if (context.render->space2D())
    return drawPlanarGizmo(context, view, snapping);

  // Several selected: the handles in the middle of them (see moveGroup()).
  const std::vector<sfs::Entity> group = movableSelection(context);
  if (context.selected().size() > 1 && !group.empty())
    return drawGroupGizmo(context, view, group, operation, snapping ? snap.data() : nullptr);

  // Manipulate a copy, so an edit can be bracketed for undo before it lands:
  // the snapshot is taken the frame the drag starts, while the scene is still
  // unchanged. The gizmo works in world space; the result goes back into the
  // transform relative to the parent.
  glm::mat4 model = sfs::hierarchy::worldMatrix(context.selection);
  const bool wasUsing = ImGuizmo::IsUsing();
  const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view.view),
                                            glm::value_ptr(view.projection),
                                            operation,
                                            m_localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                                            glm::value_ptr(model),
                                            nullptr,
                                            snapping ? snap.data() : nullptr);
  const bool usingNow = ImGuizmo::IsUsing();

  if (!wasUsing && usingNow)
    context.history->beginChange();
  if (changed)
    sfs::hierarchy::setWorldMatrix(context.selection, model);
  if (wasUsing && !usingNow)
    context.history->endChange();

  return usingNow || ImGuizmo::IsOver();
}

bool ViewportPanel::drawPlanarGizmo(EditorContext& context, const View& view, bool snapping)
{
  // The selection where its plane puts it; the handles move it across the
  // plane (an isometric sprite always faces the view, so it doesn't turn).
  const sfs::Space2D& space = *context.render->space2D();
  const bool isometric = space.projection == sfs::Space2D::Projection::Isometric;
  sfs::TransformComponent& transform = context.selection.getComponent<sfs::TransformComponent>();
  const glm::vec3 placed = space.place(transform.position2D(), transform.position.z);
  const float turn = isometric ? 0.0f : transform.yaw() * space.ySign();
  glm::mat4 model = glm::translate(glm::mat4(1.0f), placed) *
                    glm::rotate(glm::mat4(1.0f), turn, glm::vec3(0.0f, 0.0f, 1.0f)) *
                    glm::scale(glm::mat4(1.0f), glm::vec3(transform.scale2D(), 1.0f));

  ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y;
  if (m_tool == Tool::Rotate && !isometric)
    operation = ImGuizmo::ROTATE_Z;
  else if (m_tool == Tool::Scale)
    operation = ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y;
  const std::array<float, 3> turnSnap{kRotateSnap, kRotateSnap, kRotateSnap};
  const std::array<float, 3> scaleSnap{kScaleSnap, kScaleSnap, kScaleSnap};
  const float* snap = !snapping                            ? nullptr
                      : operation == ImGuizmo::ROTATE_Z ? turnSnap.data()
                      : m_tool == Tool::Scale           ? scaleSnap.data()
                                                        : nullptr; // moves snap below

  const bool wasUsing = ImGuizmo::IsUsing();
  const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view.view),
                                            glm::value_ptr(view.projection),
                                            operation,
                                            ImGuizmo::LOCAL,
                                            glm::value_ptr(model),
                                            nullptr,
                                            snap);
  const bool usingNow = ImGuizmo::IsUsing();
  if (!wasUsing && usingNow)
    context.history->beginChange();
  if (changed)
  {
    std::array<float, 3> moved{};
    std::array<float, 3> turned{};
    std::array<float, 3> scaled{};
    ImGuizmo::DecomposeMatrixToComponents(
        glm::value_ptr(model), moved.data(), turned.data(), scaled.data());
    // Back onto the game's plane, at the same height: whole pixels (flat) or
    // half tiles (isometric) when snapping.
    glm::vec2 position = space.unplace(glm::vec2(moved[0], moved[1]), transform.position.z);
    if (snapping && m_tool == Tool::Move)
    {
      const float step = isometric ? 0.5f : 1.0f;
      position = glm::round(position / step) * step;
    }
    transform.setPosition2D(position);
    if (!isometric)
      transform.setYaw(glm::radians(turned[2]) * space.ySign());
    transform.scale.x = scaled[0];
    transform.scale.y = scaled[1];
  }
  if (wasUsing && !usingNow)
    context.history->endChange();
  return usingNow || ImGuizmo::IsOver();
}

std::vector<sfs::Entity> ViewportPanel::movableSelection(const EditorContext& context)
{
  // Those with a place, but not inside another selected one (they move with
  // it).
  const std::vector<sfs::Entity> selected = context.selected();
  std::vector<sfs::Entity> movable;
  for (const sfs::Entity& entity : selected)
    if (entity.hasComponent<sfs::TransformComponent>() &&
        std::none_of(selected.begin(),
                     selected.end(),
                     [&](const sfs::Entity& outer)
                     { return outer != entity && sfs::hierarchy::isAncestor(outer, entity); }))
      movable.push_back(entity);
  return movable;
}

bool ViewportPanel::drawGroupGizmo(EditorContext& context,
                                   const View& view,
                                   const std::vector<sfs::Entity>& group,
                                   int operation,
                                   const float* snap)
{
  // The handles: at their middle (the average of where they are), lined up
  // with the world, or with the one selected last in Local. While dragging
  // they're kept from frame to frame, so each frame's change is the step
  // since the last.
  const bool wasUsing = ImGuizmo::IsUsing();
  if (!wasUsing)
  {
    glm::vec3 middle(0.0f);
    for (const sfs::Entity& entity : group)
      middle += glm::vec3(sfs::hierarchy::worldMatrix(entity)[3]);
    middle /= static_cast<float>(group.size());
    glm::mat4 turn(1.0f);
    if (m_localSpace)
    {
      const glm::mat4 world = sfs::hierarchy::worldMatrix(context.selection);
      turn = glm::mat4(glm::mat3(glm::normalize(glm::vec3(world[0])),
                                 glm::normalize(glm::vec3(world[1])),
                                 glm::normalize(glm::vec3(world[2]))));
    }
    m_groupHandles = turn;
    m_groupHandles[3] = glm::vec4(middle, 1.0f);
  }
  glm::mat4 handles = m_groupHandles;
  const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view.view),
                                            glm::value_ptr(view.projection),
                                            static_cast<ImGuizmo::OPERATION>(operation),
                                            m_localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                                            glm::value_ptr(handles),
                                            nullptr,
                                            snap);
  const bool usingNow = ImGuizmo::IsUsing();
  if (!wasUsing && usingNow)
    context.history->beginChange();
  if (changed)
  {
    // The step: how far the handles moved, turned and scaled. Each one moves
    // as far, and turns and scales as much about its own origin.
    const auto axes = [](const glm::mat4& m)
    { return glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))); };
    const auto turnOf = [&](const glm::mat4& m)
    {
      const glm::vec3 s = axes(m);
      return glm::quat_cast(glm::mat3(glm::vec3(m[0]) / s.x, glm::vec3(m[1]) / s.y, glm::vec3(m[2]) / s.z));
    };
    const glm::vec3 moved = glm::vec3(handles[3]) - glm::vec3(m_groupHandles[3]);
    const glm::quat turned = turnOf(handles) * glm::inverse(turnOf(m_groupHandles));
    const glm::vec3 scaled = axes(handles) / axes(m_groupHandles);
    for (const sfs::Entity& entity : group)
    {
      const glm::mat4 world = sfs::hierarchy::worldMatrix(entity);
      const glm::vec3 scale = axes(world) * scaled;
      const glm::quat rotation = turned * turnOf(world);
      glm::mat4 next = glm::mat4_cast(rotation);
      next[0] *= scale.x;
      next[1] *= scale.y;
      next[2] *= scale.z;
      next[3] = glm::vec4(glm::vec3(world[3]) + moved, 1.0f);
      sfs::hierarchy::setWorldMatrix(entity, next);
    }
    m_groupHandles = handles;
  }
  if (wasUsing && !usingNow)
    context.history->endChange();
  return usingNow || ImGuizmo::IsOver();
}

bool ViewportPanel::paintGrass(EditorContext& context,
                               const View& view,
                               bool hovered)
{
  const sfs::Entity lawn = context.selection;
  if (!hasGrass(lawn))
  {
    endGrassStroke(context);
    return false;
  }
  const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  if (m_brushStroke && !down)
    endGrassStroke(context);
  if (!hovered && !m_brushStroke)
    return false;

  // Where the cursor meets the grass's mesh.
  const sfs::MeshHandle mesh =
      lawn.getComponent<sfs::MeshRendererComponent>().mesh;
  sfs::MeshLibrary& meshes = context.render->meshes();
  const glm::mat4 world = sfs::hierarchy::worldMatrix(lawn);
  const ImVec2 mouse = ImGui::GetMousePos();
  const sfs::Ray ray = context.render->camera().screenRay(
      {mouse.x - view.origin.x, mouse.y - view.origin.y},
      {view.size.x, view.size.y});
  const std::optional<sfs::MeshLibrary::RayHit> hit =
      meshes.raycast(mesh, world, ray);
  if (!hit)
    return hovered;
  const glm::vec3 point = ray.origin + ray.direction * hit->distance;
  const bool erase = m_brushErase || ImGui::GetIO().KeyShift;

  // The brush's ring, flat on the ground under the cursor.
  const float radius = m_brushSize * 0.5f;
  std::array<ImVec2, 40> ring;
  int count = 0;
  for (int i = 0; i < static_cast<int>(ring.size()); ++i)
  {
    const float angle =
        6.2831853f * static_cast<float>(i) / static_cast<float>(ring.size());
    if (const auto at = toScreen(
            view,
            point + glm::vec3(std::cos(angle), std::sin(angle), 0.0f) * radius))
      ring[count++] = *at;
  }
  const ImU32 color =
      erase ? IM_COL32(235, 110, 90, 230) : IM_COL32(150, 230, 120, 230);
  ImGui::GetWindowDrawList()->AddPolyline(
      ring.data(), count, color, ImDrawFlags_Closed, 2.0f);

  // Painting while the button's down: one undo step a stroke.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
  {
    context.history->beginChange();
    m_brushStroke = true;
  }
  // Into the grass's maps as they are (unpainted grass gets them, full: what's
  // there stays until painted over), straight to the GPU; the component
  // keeps them once the stroke ends. At full strength a stroke paints fully
  // in a twelfth of a second.
  if (m_brushStroke && down)
    if (auto* grassModule = context.render->module<sfs::GrassModule>())
      grassModule->stamp(
          lawn,
          static_cast<sfs::GrassMaps::Channel>(m_brushChannel),
          point,
          radius,
          erase ? 0.0f : 1.0f,
          sfs::GrassMaps::Blend::Toward,
          std::min(m_brushStrength * 12.0f * ImGui::GetIO().DeltaTime, 1.0f));
  return true;
}

void ViewportPanel::endGrassStroke(EditorContext& context)
{
  if (!m_brushStroke)
    return;
  m_brushStroke = false;
  // What the stroke painted, into the component (and so the undo step).
  auto* grassModule = context.render->module<sfs::GrassModule>();
  if (grassModule && hasGrass(context.selection))
    if (const sfs::GrassMaps* maps = grassModule->maps(context.selection))
      context.selection.getComponent<sfs::GrassComponent>().paint =
          maps->toJson();
  context.history->endChange();
}

void ViewportPanel::pick(EditorContext& context, const View& view)
{
  const ImVec2 mouse = ImGui::GetMousePos();
  const sfs::Ray ray = context.render->camera().screenRay(
      {mouse.x - view.origin.x, mouse.y - view.origin.y}, {view.size.x, view.size.y});

  const std::optional<sfs::PickHit> hit =
      sfs::pickEntity(*context.scene, context.render->meshes(), ray, context.render);
  // Ctrl: add what's clicked to the selection, or take it out.
  if (ImGui::GetIO().KeyCtrl && hit)
  {
    std::vector<sfs::Entity> group = context.selected();
    if (const auto it = std::find(group.begin(), group.end(), hit->entity); it != group.end())
    {
      group.erase(it);
      context.selection = group.empty() ? sfs::Entity() : group.back();
    }
    else
    {
      group.push_back(hit->entity);
      context.selection = hit->entity;
    }
    context.selectionGroup = std::move(group);
    return;
  }
  context.selection = hit ? hit->entity : sfs::Entity();
}

void ViewportPanel::dropAsset(EditorContext& context, const View& view)
{
  if (!context.assets || !ImGui::BeginDragDropTarget())
    return;
  const sfs::AssetRecord* dragged = assetDrop::draggedAsset(ImGui::GetDragDropPayload(), *context.assets);
  if (dragged && assetDrop::placeable(*dragged))
  {
    // A prefab: placed where the pointer meets a surface, or the ground.
    const ImVec2 mouse = ImGui::GetMousePos();
    const sfs::Ray ray = context.render->camera().screenRay(
        {mouse.x - view.origin.x, mouse.y - view.origin.y}, {view.size.x, view.size.y});
    std::optional<glm::vec3> point;
    float distance = 0.0f;
    if (const auto hit = sfs::pickSurface(*context.scene, context.render->meshes(), ray))
      point = ray.at(hit->distance);
    else if (ray.intersectPlaneZ(0.0f, distance))
      point = ray.at(distance);
    const ImGuiPayload* payload =
        point ? ImGui::AcceptDragDropPayload(
                    assetDrop::kAssetPayload,
                    ImGuiDragDropFlags_AcceptBeforeDelivery |
                        ImGuiDragDropFlags_AcceptNoDrawDefaultRect)
              : nullptr;
    if (payload)
    {
      if (const std::optional<ImVec2> at = toScreen(view, *point))
      {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddCircle(*at, 9.0f, kDropColor, 0, 2.0f);
        draw->AddCircleFilled(*at, 3.0f, kDropColor);
      }
      if (payload->IsDelivery())
        prefabs::place(context, prefabs::nameOf(dragged->id), {}, point);
    }
  }
  else if (dragged && assetDrop::fitsMaterialSlot(*dragged))
  {
    // The surface under the cursor: its object, and which of its material
    // slots that part of the mesh is.
    const ImVec2 mouse = ImGui::GetMousePos();
    const sfs::Ray ray = context.render->camera().screenRay(
        {mouse.x - view.origin.x, mouse.y - view.origin.y}, {view.size.x, view.size.y});
    const std::optional<sfs::SurfaceHit> hit =
        sfs::pickSurface(*context.scene, context.render->meshes(), ray);
    // Accepted every frame it hovers (not only on release), so the object it
    // would land on is shown; the drop itself is the delivery.
    const ImGuiPayload* payload =
        hit ? ImGui::AcceptDragDropPayload(
                  assetDrop::kAssetPayload,
                  ImGuiDragDropFlags_AcceptBeforeDelivery |
                      ImGuiDragDropFlags_AcceptNoDrawDefaultRect)
            : nullptr;
    if (payload)
    {
      ImGui::PushClipRect(
          view.origin,
          ImVec2(view.origin.x + view.size.x, view.origin.y + view.size.y),
          true);
      drawDropTarget(context, view, *hit);
      ImGui::PopClipRect();
    }
    if (payload && payload->IsDelivery())
      if (const std::string material = assetDrop::materialFor(*context.assets, *dragged); !material.empty())
      {
        assetDrop::setSlot(
            hit->entity, hit->section, material, context.history);
        context.selection = hit->entity;
        context.selectedAsset = {};
      }
  }
  ImGui::EndDragDropTarget();
}

void ViewportPanel::drawDropTarget(const EditorContext& context, const View& view, const sfs::SurfaceHit& hit) const
{
  drawOutline(context, view, hit.entity, kDropColor, 2.5f);

  // Above the cursor (the drag's own tooltip is below it): the object, and for
  // a model with several materials, the one (slot) it would replace.
  std::string text = hit.entity.hasComponent<sfs::NameComponent>()
                         ? hit.entity.getComponent<sfs::NameComponent>().name
                         : std::string("(unnamed)");
  if (hit.entity.hasComponent<sfs::MeshRendererComponent>())
  {
    const std::vector<sfs::MeshSection> slots =
        context.render->meshes().sections(
            hit.entity.getComponent<sfs::MeshRendererComponent>().mesh);
    if (slots.size() > 1 && hit.section < slots.size())
      text += " : " + (slots[hit.section].name.empty() ? "Material " + std::to_string(hit.section + 1)
                                                       : slots[hit.section].name);
  }
  const ImVec2 mouse = ImGui::GetMousePos();
  const ImVec2 size = ImGui::CalcTextSize(text.c_str());
  const ImVec2 at(mouse.x + 12.0f, mouse.y - size.y - 14.0f);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 2.0f),
                          ImVec2(at.x + size.x + 4.0f, at.y + size.y + 2.0f),
                          IM_COL32(20, 20, 20, 200),
                          3.0f);
  drawList->AddText(at, kDropColor, text.c_str());
}

std::optional<sfs::Aabb> ViewportPanel::branchBounds(const EditorContext& context, const sfs::Entity& entity) const
{
  // Its own bounds, or (with meshes below it, e.g. a prefab's empty root)
  // those of the meshes in its branch.
  const sfs::MeshLibrary& meshes = context.render->meshes();
  sfs::Aabb meshBounds;
  for (const sfs::Entity& part : sfs::hierarchy::EntityTree(context.scene->entities()).subtree(entity))
    if (part.hasComponent<sfs::MeshRendererComponent>())
      if (const auto bounds = sfs::entityBounds(part, meshes, context.render))
        meshBounds.expand(*bounds);
  if (!meshBounds.empty())
    return meshBounds;
  return sfs::entityBounds(entity, meshes, context.render);
}

void ViewportPanel::navigate(EditorContext& context, float viewportHeight)
{
  const ImGuiIO& io = ImGui::GetIO();
  ViewportCamera::Input input;
  input.mouseDelta = {io.MouseDelta.x, io.MouseDelta.y};
  input.wheel = io.MouseWheel;
  input.pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
              (io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Right));
  input.orbit = !input.pan && ImGui::IsMouseDown(ImGuiMouseButton_Right);
  input.planar = context.render->space2D().has_value();
  m_navigation.apply(context.render->camera(), input, viewportHeight);
}

void ViewportPanel::drawSelectionOutline(const EditorContext& context, const View& view) const
{
  // Everything selected; the one selected last (the gizmo's) bolder.
  for (const sfs::Entity& entity : context.selected())
    drawOutline(context, view, entity, kSelectionColor, entity == context.selection ? 1.5f : 1.0f);
}

void ViewportPanel::drawOutline(const EditorContext& context,
                                const View& view,
                                const sfs::Entity& entity,
                                ImU32 color,
                                float thickness) const
{
  const std::optional<sfs::Aabb> bounds = branchBounds(context, entity);
  if (!bounds)
    return;

  // The box's 8 corners (bit i of the index picks min/max on axis i) and 12
  // edges (corners differing in one bit).
  std::array<std::optional<ImVec2>, 8> corners;
  for (int i = 0; i < 8; ++i)
    corners[i] = toScreen(view,
                          {(i & 1) ? bounds->max.x : bounds->min.x,
                           (i & 2) ? bounds->max.y : bounds->min.y,
                           (i & 4) ? bounds->max.z : bounds->min.z});

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  for (int a = 0; a < 8; ++a)
    for (const int bit : {1, 2, 4})
    {
      const int b = a | bit;
      if (b != a && corners[a] && corners[b])
        drawList->AddLine(*corners[a], *corners[b], color, thickness);
    }
}

void ViewportPanel::drawLightIcons(const EditorContext& context, const View& view) const
{
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  for (const sfs::Entity& entity : context.scene->entities())
  {
    if (!entity.hasComponent<sfs::PointLightComponent>() ||
        !entity.hasComponent<sfs::TransformComponent>())
      continue;

    // Where the renderer puts it: the offset turns and scales with the entity.
    const auto& light = entity.getComponent<sfs::PointLightComponent>();
    const std::optional<ImVec2> at =
        toScreen(view,
                 glm::vec3(sfs::hierarchy::worldMatrix(entity) *
                           glm::vec4(light.offset, 1.0f)));
    if (!at)
      continue;

    // A small sun: a disc with rays.
    drawList->AddCircleFilled(*at, 5.0f, kLightColor);
    for (int i = 0; i < 8; ++i)
    {
      const float angle = static_cast<float>(i) * 0.785398f;
      const ImVec2 direction{std::cos(angle), std::sin(angle)};
      drawList->AddLine(
          ImVec2(at->x + direction.x * 8.0f, at->y + direction.y * 8.0f),
          ImVec2(at->x + direction.x * 12.0f, at->y + direction.y * 12.0f),
          kLightColor,
          1.5f);
    }
  }
}

void ViewportPanel::drawCameraIcons(const EditorContext& context, const View& view) const
{
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const float aspect = view.size.x / std::max(view.size.y, 1.0f);
  const sfs::Entity gameCamera = context.render->sceneCamera();
  const glm::vec3 viewer = context.render->camera().position();

  for (const sfs::Entity& entity : context.scene->entities())
  {
    if (!entity.hasComponent<sfs::CameraComponent>() ||
        !entity.hasComponent<sfs::TransformComponent>())
      continue;

    // The camera's view, as the game would set it up.
    sfs::Camera camera;
    entity.getComponent<sfs::CameraComponent>().applyTo(
        camera, sfs::hierarchy::worldMatrix(entity));
    const glm::vec3 eye = camera.position();
    const glm::vec3 forward = camera.viewDirection();
    const glm::vec3 right = camera.orientation() * glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 up = glm::cross(right, forward);
    const float depth = std::max(glm::distance(eye, viewer) * kCameraIconScale, 0.3f);

    // A pyramid from the eye to a rectangle the shape of the view (a box for
    // orthographic), plus a notch marking up.
    const bool orthographic = camera.projection == sfs::Camera::Projection::Orthographic;
    const float halfHeight =
        orthographic ? std::min(camera.orthoHeight * 0.5f, depth)
                     : std::tan(camera.fieldOfView * 0.5f) * depth;
    const float halfWidth = halfHeight * aspect;
    const glm::vec3 centre = eye + forward * depth;
    const std::array<glm::vec3, 4> far{
        centre + right * halfWidth + up * halfHeight,
        centre - right * halfWidth + up * halfHeight,
        centre - right * halfWidth - up * halfHeight,
        centre + right * halfWidth - up * halfHeight};
    const std::array<glm::vec3, 4> near =
        orthographic ? std::array<glm::vec3, 4>{far[0] - forward * depth,
                                                far[1] - forward * depth,
                                                far[2] - forward * depth,
                                                far[3] - forward * depth}
                     : std::array<glm::vec3, 4>{eye, eye, eye, eye};

    const ImU32 color = entity == gameCamera ? kCameraColor : kInactiveCameraColor;
    const auto line = [&](const glm::vec3& a, const glm::vec3& b)
    {
      const std::optional<ImVec2> from = toScreen(view, a);
      const std::optional<ImVec2> to = toScreen(view, b);
      if (from && to)
        drawList->AddLine(*from, *to, color, 1.5f);
    };
    for (int i = 0; i < 4; ++i)
    {
      line(near[i], far[i]);
      line(far[i], far[(i + 1) % 4]);
      if (orthographic)
        line(near[i], near[(i + 1) % 4]);
    }
    const glm::vec3 notch = centre + up * (halfHeight * 1.4f);
    line(far[0], notch);
    line(far[1], notch);
  }
}

void ViewportPanel::drawColliders(const EditorContext& context, const View& view) const
{
  if (!context.hasSelection() || !context.selection.hasComponent<sfs::TransformComponent>())
    return;

  const sfs::Entity& entity = context.selection;
  const glm::mat4 world = sfs::hierarchy::worldMatrix(entity);
  const glm::vec3 scale{glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                        glm::length(glm::vec3(world[2]))};
  // Turned with the entity but unscaled: scale goes into the sizes, the way
  // the physics bakes it (a sphere stays round).
  glm::mat4 turned = world;
  for (int axis = 0; axis < 3; ++axis)
    turned[axis] = scale[axis] > 0.0f ? world[axis] / scale[axis] : world[axis];

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  ImU32 color = kColliderColor;
  const auto line = [&](const glm::vec3& a, const glm::vec3& b)
  {
    const std::optional<ImVec2> from = toScreen(view, glm::vec3(turned * glm::vec4(a, 1.0f)));
    const std::optional<ImVec2> to = toScreen(view, glm::vec3(turned * glm::vec4(b, 1.0f)));
    if (from && to)
      drawList->AddLine(*from, *to, color, 1.5f);
  };
  // A circle (or the arc from `start` to `end` turns) in the plane of axes u,
  // v.
  const auto circle = [&](const glm::vec3& centre,
                          const glm::vec3& u,
                          const glm::vec3& v,
                          float radius,
                          float start = 0.0f,
                          float end = 1.0f)
  {
    constexpr int kSegments = 32;
    const int segments = std::max(4, static_cast<int>(kSegments * (end - start)));
    for (int i = 0; i < segments; ++i)
    {
      const float a0 = 6.2831853f * (start + (end - start) * i / segments);
      const float a1 = 6.2831853f * (start + (end - start) * (i + 1) / segments);
      line(centre + radius * (std::cos(a0) * u + std::sin(a0) * v),
           centre + radius * (std::cos(a1) * u + std::sin(a1) * v));
    }
  };
  const glm::vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};

  if (entity.hasComponent<sfs::BoxCollider3DComponent>())
  {
    const auto& box = entity.getComponent<sfs::BoxCollider3DComponent>();
    color = box.trigger ? kTriggerColor : kColliderColor;
    const glm::vec3 half = box.size * scale * 0.5f;
    const glm::vec3 centre = box.offset * scale;
    const auto corner = [&](int i)
    { return centre + glm::vec3((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z); };
    for (int a = 0; a < 8; ++a)
      for (const int bit : {1, 2, 4})
        if (!(a & bit))
          line(corner(a), corner(a | bit));
  }

  if (entity.hasComponent<sfs::SphereCollider3DComponent>())
  {
    const auto& sphere = entity.getComponent<sfs::SphereCollider3DComponent>();
    color = sphere.trigger ? kTriggerColor : kColliderColor;
    const float radius = sphere.radius * std::max({scale.x, scale.y, scale.z});
    const glm::vec3 centre = sphere.offset * scale;
    circle(centre, x, y, radius);
    circle(centre, x, z, radius);
    circle(centre, y, z, radius);
  }

  if (entity.hasComponent<sfs::CapsuleCollider3DComponent>())
  {
    const auto& capsule = entity.getComponent<sfs::CapsuleCollider3DComponent>();
    color = capsule.trigger ? kTriggerColor : kColliderColor;
    const float radius = capsule.radius * std::max(scale.x, scale.y);
    const float half = std::max(capsule.height * scale.z * 0.5f - radius, 0.0f);
    const glm::vec3 centre = capsule.offset * scale;
    const glm::vec3 top = centre + z * half;
    const glm::vec3 bottom = centre - z * half;
    circle(top, x, y, radius);
    circle(bottom, x, y, radius);
    for (const glm::vec3& side : {x, -x, y, -y})
      line(top + side * radius, bottom + side * radius);
    // The rounded ends: half circles over the top and under the bottom.
    circle(top, x, z, radius, 0.0f, 0.5f);
    circle(top, y, z, radius, 0.0f, 0.5f);
    circle(bottom, x, z, radius, 0.5f, 1.0f);
    circle(bottom, y, z, radius, 0.5f, 1.0f);
  }
}
