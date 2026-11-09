#pragma once

#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"
#include "engine/runtime/ui/fontAtlas.h"
#include "engine/runtime/ui/imageAtlas.h"
#include "engine/runtime/ui/layout.h"

#include <chrono>
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
 * on the screen (in order, by Layer::order) and drawn over the rest (after
 * bloom and depth of field). Retained: each frame a layer's body() is made
 * (unless its Layer::inputs() say it's as it was) and only what's changed in
 * it is laid out and drawn again (ui::Cache); a layer the same as last frame
 * costs next to nothing.
 *
 * Sizes are in points: pixels at 1080 lines high, so UI is as big on any
 * screen (scale()). It also takes the scene's input first (handleInput()):
 * clicks on a layer's buttons call their actions, the wheel over a scroll
 * view scrolls it, and the mouse, while it's over UI or pressed there, isn't
 * the game's (MouseInput::blocked()). Each layer's scroll offsets and open
 * sections (ui::State) are kept while the layer lives.
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
   * viewport), whether it's in the view, the left button's state, and the
   * wheel's turn (notches; +y away from you, +x right). Whether the mouse is
   * the UI's now (the game's mouse is blocked).
   */
  bool pointer(glm::vec2 position,
               bool inView,
               bool pressed,
               bool released,
               bool held,
               glm::vec2 wheel = glm::vec2(0.0f));

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
   * (drawn once), in the module's atlas; null if there's none, or the mesh
   * isn't loaded yet.
   */
  const ui::SourceImage* image(const std::string& source, bool mesh) override;

  /**
   * What was laid out last frame, every layer's, front last (for tests and
   * tools; ids are each layer's own).
   */
  const std::vector<ui::Hit>& hits();

private:
  // A font's file as a path to open: an asset ("asset:<id>"), the project's
  // assets folder, or the engine's (beside the program).
  std::string resolveFont(const std::string& file) const;
  // A file the UI reads (a font, an image) as a path to open: an asset
  // ("asset:<id>"), the project's folder, or beside the program; empty if
  // there's none.
  std::string resolveFile(const std::string& file) const;
  // What a layer keeps from frame to frame.
  struct LayerUi
  {
    ui::State state;     // scroll offsets, open sections
    ui::Cache cache;     // its layout, kept
    ui::View tree;       // its newest body (hits' sources are found in it)
    uint64_t inputs = 0; // Layer::inputs() when `tree` was made (0: none)
    bool made = false;   // `tree` has been made
  };
  // Something under the pointer: a layer's hit.
  struct Found
  {
    const ui::Layer* layer = nullptr;
    const ui::Hit* hit = nullptr;
  };
  // The front hit there (with help, for helpAt()).
  Found hitAt(glm::vec2 point) const;
  Found helpAt(glm::vec2 point) const;
  // A hit's node in its layer's newest tree (its callbacks' source).
  const ui::Node* sourceOf(const Found& found) const;
  // The pointer as `layer`'s layout sees it.
  ui::Pointer pointerFor(const ui::Layer* layer) const;

  Scene& m_scene;
  ui::Theme m_theme;
  ui::FontAtlas m_fonts;
  MeshLibrary* m_meshes = nullptr; // the frame's (for mesh icons)
  // Images by source ("f:" a file, "m:" a mesh), in the atlas; null: there's
  // none.
  ui::ImageAtlas m_atlas;
  std::unordered_map<std::string, const ui::SourceImage*> m_images;
  float m_scale = 1.0f;
  float m_userScale = 1.0f;
  float m_fixedScale = 0.0f;
  std::function<std::vector<ui::Layer*>()> m_layerSource;
  ui::Layer* m_overlay = nullptr;
  bool m_recordPlaced = false;
  std::vector<ui::Placed> m_placed;
  glm::vec2 m_screen{0.0f};

  std::unordered_map<const ui::Layer*, LayerUi> m_layers;
  // Last frame's layers, back to front (their hits are their caches').
  std::vector<std::pair<const ui::Layer*, LayerUi*>> m_drawn;
  std::vector<ui::Hit> m_allHits; // hits(), gathered when asked
  std::chrono::steady_clock::time_point m_lastLayout{};
  glm::vec2 m_position{-1.0e9f}; // the pointer's
  // What it's over and holding: a layer's hit's id.
  const ui::Layer* m_hoveredLayer = nullptr;
  ui::Id m_hovered = 0;
  const ui::Layer* m_pressedLayer = nullptr;
  ui::Id m_pressed = 0;
  bool m_captured = false; // pressed on UI: the mouse is the UI's until let go
  double m_hoveredFor = 0.0; // seconds over the same thing (for its help)
  uint64_t m_hoverId = 0;    // what m_hoveredFor is timing
  uint64_t m_helpId = 0;     // what's under the pointer with help
};

} // namespace sfs
