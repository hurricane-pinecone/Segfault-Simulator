#include "panels/viewportPanel.h"

#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"

#include "engine/core/components/pointLight3DComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/runtime/rendering3d/picking.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/webgpu/webGpuRenderBackend.h"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/gtc/type_ptr.hpp"

#include "imguizmo/ImGuizmo.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr float kMoveSnap = 0.5f;    // world units
constexpr float kRotateSnap = 15.0f; // degrees
constexpr float kScaleSnap = 0.1f;

constexpr ImU32 kSelectionColor = IM_COL32(255, 170, 40, 255);
constexpr ImU32 kLightColor = IM_COL32(255, 220, 120, 255);

// Translation, rotation and scale from a TRS matrix (no shear).
void decompose(const glm::mat4& matrix, sfs::Transform3DComponent& transform)
{
  transform.position = glm::vec3(matrix[3]);
  const glm::vec3 scale{glm::length(glm::vec3(matrix[0])),
                        glm::length(glm::vec3(matrix[1])),
                        glm::length(glm::vec3(matrix[2]))};
  const glm::mat3 rotation{glm::vec3(matrix[0]) / scale.x,
                           glm::vec3(matrix[1]) / scale.y,
                           glm::vec3(matrix[2]) / scale.z};
  transform.rotation = glm::normalize(glm::quat_cast(rotation));
  transform.scale = scale;
}
} // namespace

ViewportPanel::ViewportPanel(sfs::WebGpuRenderBackend& backend)
    : Panel("Viewport"), m_backend(backend), m_target(backend.context())
{
  m_backend.setSceneTarget(&m_target);
}

ViewportPanel::~ViewportPanel() { m_backend.setSceneTarget(nullptr); }

int ViewportPanel::windowFlags() const
{
  // The image fills the panel; scrolling would only fight camera zoom. Keys
  // are for the viewport (shortcuts) and the game, not ImGui navigation.
  return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
         ImGuiWindowFlags_NoNavInputs;
}

void ViewportPanel::update(EditorContext&)
{
  // Resize before the scene renders: the frame's ImGui draw (later this frame)
  // samples the target, so it must not be replaced after the panel is drawn.
  m_target.resize(open() ? m_desiredSize : glm::ivec2(0, 0));
}

ViewportPanel::View ViewportPanel::currentView(const EditorContext& context,
                                               ImVec2 origin,
                                               ImVec2 size) const
{
  const sfs::Camera3D& camera = context.render->camera();
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
  m_desiredSize = {static_cast<int>(available.x), static_cast<int>(available.y)};

  if (!m_target.valid() || m_target.size() != m_desiredSize || !context.render)
  {
    // First frame, or mid-resize: the correctly sized target arrives next
    // frame.
    ImGui::Dummy(available);
    return;
  }

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(m_target.imguiView())),
               available);
  const bool imageHovered = ImGui::IsItemHovered();
  const View view = currentView(context, origin, available);

  drawLightIcons(context, view);
  drawSelectionOutline(context, view);
  const bool gizmoActive = drawGizmo(context, view);

  if (context.playing)
  {
    // The game owns the camera and the keyboard while the viewport has focus
    // (like a game view); click into it to play.
    const bool focused = ImGui::IsWindowFocused();
    context.gameHasInput = focused;
    if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      ImGui::SetWindowFocus();
    const ImU32 border = focused ? IM_COL32(90, 200, 120, 255) : IM_COL32(200, 160, 60, 255);
    ImGui::GetWindowDrawList()->AddRect(origin, ImVec2(origin.x + available.x, origin.y + available.y),
                                        border, 0.0f, 0, 2.0f);
    if (!focused)
    {
      ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + available.y - 26.0f));
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

  ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 8.0f));
  drawToolbar();
}

void ViewportPanel::drawToolbar()
{
  const auto toolButton = [&](const char* label, Tool tool, const char* tip)
  {
    const bool active = m_tool == tool;
    if (active)
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
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

  if (ImGui::IsKeyPressed(ImGuiKey_F, false) && context.hasSelection() &&
      context.selection.hasComponent<sfs::Transform3DComponent>())
    m_navigation.focus(context.render->camera(),
                       context.selection.getComponent<sfs::Transform3DComponent>().position);
}

bool ViewportPanel::drawGizmo(EditorContext& context, const View& view)
{
  if (!context.hasSelection() || !context.selection.hasComponent<sfs::Transform3DComponent>())
    return false;

  auto& transform = context.selection.getComponent<sfs::Transform3DComponent>();

  ImGuizmo::SetDrawlist();
  ImGuizmo::SetRect(view.origin.x, view.origin.y, view.size.x, view.size.y);
  ImGuizmo::SetOrthographic(context.render->camera().projection ==
                            sfs::Camera3D::Projection::Orthographic);

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

  // Manipulate a copy, so an edit can be bracketed for undo before it lands:
  // the snapshot is taken the frame the drag starts, while the scene is still
  // unchanged.
  glm::mat4 model = transform.matrix();
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
    decompose(model, transform);
  if (wasUsing && !usingNow)
    context.history->endChange();

  return usingNow || ImGuizmo::IsOver();
}

void ViewportPanel::pick(EditorContext& context, const View& view)
{
  const ImVec2 mouse = ImGui::GetMousePos();
  const sfs::Ray3D ray = context.render->camera().screenRay(
      {mouse.x - view.origin.x, mouse.y - view.origin.y}, {view.size.x, view.size.y});

  const std::optional<sfs::PickHit> hit =
      sfs::pickEntity(*context.scene, context.render->meshes(), ray);
  context.selection = hit ? hit->entity : sfs::Entity();
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
  m_navigation.apply(context.render->camera(), input, viewportHeight);
}

void ViewportPanel::drawSelectionOutline(const EditorContext& context, const View& view) const
{
  if (!context.hasSelection())
    return;
  const std::optional<sfs::Aabb> bounds =
      sfs::entityBounds(context.selection, context.render->meshes());
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
        drawList->AddLine(*corners[a], *corners[b], kSelectionColor, 1.5f);
    }
}

void ViewportPanel::drawLightIcons(const EditorContext& context, const View& view) const
{
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  for (const sfs::Entity& entity : context.scene->entities())
  {
    if (!entity.hasComponent<sfs::PointLight3DComponent>() ||
        !entity.hasComponent<sfs::Transform3DComponent>())
      continue;

    const auto& transform = entity.getComponent<sfs::Transform3DComponent>();
    const auto& light = entity.getComponent<sfs::PointLight3DComponent>();
    const std::optional<ImVec2> at = toScreen(view, transform.position + light.offset);
    if (!at)
      continue;

    // A small sun: a disc with rays.
    drawList->AddCircleFilled(*at, 5.0f, kLightColor);
    for (int i = 0; i < 8; ++i)
    {
      const float angle = static_cast<float>(i) * 0.785398f;
      const ImVec2 direction{std::cos(angle), std::sin(angle)};
      drawList->AddLine(ImVec2(at->x + direction.x * 8.0f, at->y + direction.y * 8.0f),
                        ImVec2(at->x + direction.x * 12.0f, at->y + direction.y * 12.0f),
                        kLightColor,
                        1.5f);
    }
  }
}
