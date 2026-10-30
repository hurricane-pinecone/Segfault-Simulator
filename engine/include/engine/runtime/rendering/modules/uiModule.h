#pragma once

#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"
#include "engine/runtime/ui/fontAtlas.h"
#include "engine/runtime/ui/layout.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

class Input;
class MeshLibrary;
class Scene;
namespace ui
{
class Layer;
}

/**
 * Render module drawing a scene's UI: every ui::Layer script in it, laid out
 * on the screen each frame (in order, by Layer::order) and drawn over the rest
 * (after bloom and depth of field).
 *
 * Sizes are in points: pixels at 1080 lines high, so UI is as big on any
 * screen (scale()). It also takes the scene's input first (handleInput()):
 * clicks on a layer's buttons call their actions, and the mouse, while it's
 * over UI or pressed there, isn't the game's (MouseInput::blocked()).
 */
class UiModule : public CommandModule<RenderContext, UiDrawCommand>,
                 public ui::ImageSource
{
public:
  explicit UiModule(Scene& scene);

  void update(double deltaTime) override;
  void computeCommands(const RenderContext& context) override;

  /** Clicks on UI, and the mouse kept from the game while it's the UI's. */
  void handleInput(Input& input);

  /**
   * The mouse as handleInput() takes it: where it is (pixels in the
   * viewport), whether it's in the view, and the left button's state.
   * Whether the mouse is the UI's now (the game's mouse is blocked).
   */
  bool pointer(glm::vec2 position,
               bool inView,
               bool pressed,
               bool released,
               bool held);

  /**
   * Draw these layers instead of the scene's (the editor's UI preview);
   * an empty function goes back to the scene's.
   */
  void setLayers(std::function<std::vector<ui::Layer*>()> layers)
  {
    m_layerSource = std::move(layers);
  }

  /**
   * A layer of the host's (not the scene's) drawn over the scene's, e.g. the
   * game's developer console; null for none. It must outlive its use here.
   */
  void setOverlay(ui::Layer* layer) { m_overlay = layer; }

  /**
   * Pixels a point, fixed (the editor's UI Preview: points as its panel's);
   * 0 goes back to following the screen's height.
   */
  void setPixelsPerPoint(float pixels) { m_fixedScale = pixels; }

  /**
   * How big a view would be laid out in `offer` pixels (its own size, or
   * the offer along a way it stretches), at the fixed pixels a point.
   */
  glm::vec2 fit(const ui::View& view, glm::vec2 offer);

  /** Keep where every view went (placed()), for tools. */
  void setRecordPlaced(bool record) { m_recordPlaced = record; }
  /** Where every view went last frame, layer by layer, if recorded. */
  const std::vector<ui::Placed>& placed() const { return m_placed; }

  /** The colours and default font (Theme::font) layers are drawn with. */
  ui::Theme& theme() { return m_theme; }

  /** Pixels a point, now (the screen's height / 1080, x setScale()). */
  float scale() const { return m_scale; }
  /** Bigger or smaller UI (1: as designed); a game's UI scale setting. */
  void setScale(float scale) { m_userScale = scale; }
  /** What setScale() set. */
  float userScale() const { return m_userScale; }

  /**
   * An Image's picture: an image file (loaded once), or a mesh's icon
   * (drawn once); null if there's none, or the mesh isn't loaded yet.
   */
  const UiTexture* image(const std::string& source, bool mesh) override;

  /** What was laid out last frame (for tests and tools). */
  const std::vector<ui::Hit>& hits() const { return m_hits; }

private:
  // A font's file as a path to open: an asset ("asset:<id>"), the project's
  // assets folder, or the engine's (beside the program).
  std::string resolveFont(const std::string& file) const;
  // A file the UI reads (a font, an image) as a path to open: an asset
  // ("asset:<id>"), the project's folder, or beside the program; empty if
  // there's none.
  std::string resolveFile(const std::string& file) const;
  const ui::Hit* hitAt(glm::vec2 point) const;
  const ui::Hit* helpAt(glm::vec2 point) const;

  Scene& m_scene;
  ui::Theme m_theme;
  ui::FontAtlas m_fonts;
  MeshLibrary* m_meshes = nullptr; // the frame's (for mesh icons)
  // Images by source ("f:" a file, "m:" a mesh); null: there's none.
  std::unordered_map<std::string, std::unique_ptr<UiTexture>> m_images;
  float m_scale = 1.0f;
  float m_userScale = 1.0f;
  float m_fixedScale = 0.0f;
  std::function<std::vector<ui::Layer*>()> m_layerSource;
  ui::Layer* m_overlay = nullptr;
  bool m_recordPlaced = false;
  std::vector<ui::Placed> m_placed;
  glm::vec2 m_screen{0.0f};

  std::vector<ui::Hit> m_hits; // last frame's, front last
  ui::Pointer m_pointer;
  bool m_captured = false; // pressed on UI: the mouse is the UI's until let go
  double m_hoveredFor = 0.0; // seconds over the same thing (for its help)
  std::string m_hoverId;     // what m_hoveredFor is timing
  std::string m_helpId;      // what's under the pointer with help
};

} // namespace sfs
