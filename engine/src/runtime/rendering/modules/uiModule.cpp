#include "engine/runtime/rendering/modules/uiModule.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/logger/logger.h"
#include "engine/core/rendering/meshPreview.h"
#include "engine/core/util/cpuTiming.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/runtime/ui/layer.h"

#include <SDL_filesystem.h>
#include <SDL_image.h>
#include <SDL_stdinc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace sfs
{

namespace
{
constexpr double kHelpDelay =
    0.5; // seconds over something before its help shows
constexpr float kWheelStep = 60.0f; // points a scroll view goes a notch
} // namespace

UiModule::UiModule(Scene& scene)
    : m_scene(scene),
      m_fonts([this](const std::string& file) { return resolveFont(file); },
              ui::Theme().font)
{
  // Mesh icons (Image::mesh) are drawn from meshes' vertices: kept on the CPU
  // from here on (a little memory, as the editor keeps them for its
  // previews).
  MeshLibrary::setKeepVertices(true);
}

std::string UiModule::resolveFont(const std::string& file) const
{
  return resolveFile(file);
}

const ui::SourceImage* UiModule::image(const std::string& source, bool mesh)
{
  const std::string key = (mesh ? "m:" : "f:") + source;
  if (const auto it = m_images.find(key); it != m_images.end())
    return it->second;

  // Each is put in the atlas (small ones share its pages, with their smaller
  // levels: see ui::ImageAtlas), once.
  glm::ivec2 size{0, 0};
  std::vector<uint8_t> pixels;
  if (mesh)
  {
    // Drawn once, at a size that stays sharp up to a large icon.
    // Not cached till it's drawn: a mesh may be added or loaded later.
    if (!m_meshes)
      return nullptr;
    const MeshHandle handle =
        source.starts_with('#') ? MeshHandle{static_cast<uint32_t>(std::strtoul(
                                      source.c_str() + 1, nullptr, 10))}
                                : m_meshes->find(source);
    if (!handle.valid() || m_meshes->isMissing(handle))
      return nullptr;
    const std::optional<MeshData> data = m_meshes->meshData(handle);
    if (!data || data->empty())
      return nullptr;
    constexpr int kIconSize = 128;
    size = {kIconSize, kIconSize};
    pixels = meshPreview::render(*data, kIconSize);
  }
  else
  {
    const std::string path = resolveFile(source);
    SDL_Surface* loaded = path.empty() ? nullptr : IMG_Load(path.c_str());
    SDL_Surface* rgba =
        loaded ? SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0)
               : nullptr;
    if (loaded)
      SDL_FreeSurface(loaded);
    if (!rgba)
    {
      LOG_ERROR("UI image '" + source + "' can't be loaded" +
                (path.empty() ? " (no such file)" : ""));
      m_images[key] = nullptr;
      return nullptr;
    }
    size = {rgba->w, rgba->h};
    pixels.resize(std::size_t(rgba->w) * rgba->h * 4);
    SDL_LockSurface(rgba);
    for (int y = 0; y < rgba->h; ++y)
      std::memcpy(&pixels[std::size_t(y) * rgba->w * 4],
                  static_cast<const uint8_t*>(rgba->pixels) +
                      std::size_t(y) * rgba->pitch,
                  std::size_t(rgba->w) * 4);
    SDL_UnlockSurface(rgba);
    SDL_FreeSurface(rgba);
  }
  if (pixels.size() != std::size_t(size.x) * size.y * 4)
    return nullptr;
  return m_images[key] = m_atlas.add(pixels, size);
}

std::string UiModule::resolveFile(const std::string& file) const
{
  namespace fs = std::filesystem;
  std::error_code error;
  const AssetDatabase* assets = scriptableObjects().catalogue();
  // A font asset, by id.
  if (file.starts_with("asset:"))
  {
    if (const auto id = AssetId::parse(file.substr(6)); assets && id)
      if (const AssetRecord* record = assets->find(*id))
        return assets->absolutePath(*record).string();
    return {};
  }
  // In the project's assets folder, as it is, or beside the program (the
  // engine's own fonts).
  std::vector<fs::path> places;
  if (assets)
    places.push_back(assets->root() / file);
  places.push_back(fs::path(file));
  if (char* base = SDL_GetBasePath())
  {
    places.push_back(fs::path(base) / file);
    SDL_free(base);
  }
  for (const fs::path& place : places)
    if (fs::is_regular_file(place, error))
      return place.string();
  return {};
}

void UiModule::update(double deltaTime)
{
  // How long the pointer's been over the same thing, for its help.
  if (m_helpId == m_hoverId)
    m_hoveredFor += deltaTime;
  else
  {
    m_hoverId = m_helpId;
    m_hoveredFor = 0.0;
  }
}

void UiModule::computeCommands(const RenderContext& context)
{
  flush();
  m_fonts.beginFrame(); // last frame's text is drawn by now
  m_meshes = context.meshes;
  m_screen = glm::vec2(context.viewportSize);
  m_scale = m_fixedScale > 0.0f
                ? m_fixedScale
                : std::clamp(m_screen.y / 1080.0f, 0.5f, 4.0f) * m_userScale;

  // How long since the last layout, for easing (real time: it eases while
  // the game's paused too).
  const auto now = std::chrono::steady_clock::now();
  const float deltaTime =
      m_lastLayout == std::chrono::steady_clock::time_point{}
          ? 0.0f
          : std::clamp(std::chrono::duration<float>(now - m_lastLayout).count(),
                       0.0f,
                       0.1f);
  m_lastLayout = now;

  // The scene's layers (or the ones given), back to front.
  std::vector<ui::Layer*> all =
      m_layerSource ? m_layerSource() : m_scene.findObjects<ui::Layer>();
  if (m_overlay)
    all.push_back(m_overlay);
  std::vector<ui::Layer*> layers;
  for (ui::Layer* layer : all)
    if (layer && layer->visible && layer != m_overlay)
      layers.push_back(layer);
  // (Hidden layers keep what they had; gone ones don't.)
  m_drawn.clear();
  std::erase_if(
      m_layers,
      [&](const auto& entry)
      { return std::find(all.begin(), all.end(), entry.first) == all.end(); });
  std::stable_sort(layers.begin(),
                   layers.end(),
                   [](const ui::Layer* a, const ui::Layer* b)
                   { return a->order < b->order; });
  if (m_overlay && m_overlay->visible)
    layers.push_back(m_overlay);

  m_placed.clear();
  if (layers.empty() || m_screen.x <= 0.0f || m_screen.y <= 0.0f)
    return;
  auto list = std::make_shared<UiDrawList>();
  for (ui::Layer* layer : layers)
  {
    LayerUi& kept = m_layers[layer];
    // Its body: made again unless it says what it shows is as it was.
    const uint64_t inputs = layer->inputs();
    if (!kept.made || inputs == 0 || inputs != kept.inputs)
    {
      const cpuTiming::Scope timed(
          cpuTiming::enabled() ? cpuTiming::label("ui/body ", typeid(*layer))
                               : nullptr);
      kept.tree = layer->body();
      kept.inputs = inputs;
      kept.made = true;
    }
    {
      const cpuTiming::Scope timed(
          cpuTiming::enabled() ? cpuTiming::label("ui/layout ", typeid(*layer))
                               : nullptr);
      kept.state.advance(deltaTime);
      // (For measuring what keeping saves: SFS_UI_NO_CACHE lays every layer
      // out in full each frame.)
      static const bool uncached = std::getenv("SFS_UI_NO_CACHE") != nullptr;
      if (uncached)
        kept.cache.clear();
      ui::layout(kept.tree,
                 m_screen,
                 m_scale,
                 m_theme,
                 m_fonts,
                 pointerFor(layer),
                 kept.state,
                 kept.cache,
                 this,
                 m_recordPlaced ? &m_placed : nullptr);
    }
    m_drawn.emplace_back(layer, &kept);
    if (!kept.cache.quads()->empty())
      list->parts.push_back(kept.cache.quads());
  }

  // Help for what the pointer's been over a moment.
  if (const Found help = helpAt(m_position);
      help.hit && m_hoveredFor >= kHelpDelay && m_pressed == 0)
    ui::tooltip(help.hit->help,
                m_position,
                m_screen,
                m_scale,
                m_theme,
                m_fonts,
                list->quads);

  if (!list->quads.empty() || !list->parts.empty())
    m_commands.push_back(UiDrawCommand{std::move(list)});
}

const std::vector<ui::Hit>& UiModule::hits()
{
  m_allHits.clear();
  for (const auto& [layer, kept] : m_drawn)
    m_allHits.insert(
        m_allHits.end(), kept->cache.hits().begin(), kept->cache.hits().end());
  return m_allHits;
}

ui::Pointer UiModule::pointerFor(const ui::Layer* layer) const
{
  ui::Pointer pointer;
  pointer.position = m_position;
  pointer.hovered = m_hoveredLayer == layer ? m_hovered : 0;
  pointer.pressed = m_pressedLayer == layer ? m_pressed : 0;
  return pointer;
}

glm::vec2 UiModule::fit(const ui::View& view, glm::vec2 offer)
{
  const float scale = m_fixedScale > 0.0f ? m_fixedScale : m_scale;
  return ui::fit(view, offer, scale, m_theme, m_fonts, this);
}

UiModule::Found UiModule::hitAt(glm::vec2 point) const
{
  // The front one: the last laid out there.
  for (auto layer = m_drawn.rbegin(); layer != m_drawn.rend(); ++layer)
  {
    const std::vector<ui::Hit>& hits = layer->second->cache.hits();
    for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit)
      if (hit->rect.contains(point))
        return {layer->first, &*hit};
  }
  return {};
}

UiModule::Found UiModule::helpAt(glm::vec2 point) const
{
  // The front one with help: a button's, or a view's it's inside.
  for (auto layer = m_drawn.rbegin(); layer != m_drawn.rend(); ++layer)
  {
    const std::vector<ui::Hit>& hits = layer->second->cache.hits();
    for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit)
      if (!hit->help.empty() && hit->rect.contains(point))
        return {layer->first, &*hit};
  }
  return {};
}

const ui::Node* UiModule::sourceOf(const Found& found) const
{
  const auto kept = m_layers.find(found.layer);
  if (!found.hit || kept == m_layers.end())
    return nullptr;
  return ui::nodeAt(*kept->second.tree.node(), found.hit->source);
}

void UiModule::handleInput(Input& input)
{
  MouseInput& mouse = input.mouse();
  // (For screenshots of a hover or press: SFS_UI_POINTER=x,y pins the
  // pointer there, in points from the top left; x,y,down holds it down too.)
  static const char* const pinned = std::getenv("SFS_UI_POINTER");
  if (pinned)
  {
    float x = 0.0f, y = 0.0f;
    char down[8] = {};
    const int read = std::sscanf(pinned, "%f,%f,%7s", &x, &y, down);
    if (read >= 2)
    {
      // (Pressed again until something's held: the UI may not be there yet.)
      const bool hold = read == 3;
      pointer(
          glm::vec2(x, y) * m_scale, true, hold && m_pressed == 0, false, hold);
      mouse.block();
      return;
    }
  }
  // (For testing clicks: SFS_UI_CLICK=help,frame clicks the middle of what
  // has that help (a part of its text) from that frame on (pressed, then let
  // go two frames later, through the same hit testing as the mouse's), logged
  // with what it found there.)
  static const char* const click = std::getenv("SFS_UI_CLICK");
  if (click)
  {
    static int frame = 0, pressedAt = -1;
    static bool done = false;
    static glm::vec2 at{0.0f};
    const std::string spec = click;
    const std::size_t comma = spec.rfind(',');
    const std::string help = spec.substr(0, comma);
    const int from =
        comma == std::string::npos ? 0 : std::atoi(spec.c_str() + comma + 1);
    ++frame;
    if (!done && pressedAt < 0 && frame >= from)
      for (const auto& [layer, kept] : m_drawn)
        for (const ui::Hit& each : kept->cache.hits())
          if (pressedAt < 0 && each.help.find(help) != std::string::npos)
          {
            at = glm::vec2(each.rect.x + each.rect.width * 0.5f,
                           each.rect.y + each.rect.height * 0.5f);
            const Found front = hitAt(at);
            LOG_INFO("SFS_UI_CLICK '" + help + "': pressing at " +
                     std::to_string(at.x) + "," + std::to_string(at.y) +
                     ", in front there: " +
                     (front.hit ? "'" + front.hit->help + "'" +
                                      (front.hit->action ? " (clickable)"
                                                         : " (not clickable)")
                                : std::string("nothing")));
            // ...and everything there, front to back.
            std::string there;
            for (auto l = m_drawn.rbegin(); l != m_drawn.rend(); ++l)
              for (auto h = l->second->cache.hits().rbegin();
                   h != l->second->cache.hits().rend();
                   ++h)
                if (h->rect.contains(at))
                  there += std::string(there.empty() ? "" : ", ") +
                           (h->action ? "clickable" : "not clickable") +
                           (h->help.empty() ? "" : " '" + h->help + "'");
            LOG_INFO("SFS_UI_CLICK '" + help +
                     "': there, front to back: " + there);
            pointer(at, true, true, false, true);
            pressedAt = frame;
          }
    if (pressedAt >= 0 && !done)
    {
      if (frame >= pressedAt + 2)
      {
        pointer(at, true, false, true, false);
        done = true;
        LOG_INFO("SFS_UI_CLICK '" + help + "': let go");
      }
      else
        pointer(at, true, false, false, true);
      mouse.block();
      return;
    }
  }
  // The UI's mouse: held since a press on it, or over it with nothing held
  // (a drag the game started carries on across it).
  const bool held = mouse.mouseHeld(MouseButton::Left) ||
                    mouse.mouseHeld(MouseButton::Right) ||
                    mouse.mouseHeld(MouseButton::Middle);
  if (pointer(mouse.getPosition(),
              mouse.inView(),
              mouse.mousePressed(MouseButton::Left),
              mouse.mouseReleased(MouseButton::Left),
              held,
              glm::vec2(static_cast<float>(mouse.getScrollX()),
                        static_cast<float>(mouse.getScrollY()))))
    mouse.block(); // (the wheel too: a scroll view's, or nothing's)
}

bool UiModule::pointer(glm::vec2 position,
                       bool inView,
                       bool pressed,
                       bool released,
                       bool held,
                       glm::vec2 wheel)
{
  m_position = position;
  const Found hit = inView ? hitAt(m_position) : Found{};
  m_hoveredLayer = hit.layer;
  m_hovered = hit.hit ? hit.hit->id : 0;
  // What's under the pointer for its help (timed in update()).
  const Found help = inView ? helpAt(m_position) : Found{};
  m_helpId =
      help.hit ? help.hit->id ^ reinterpret_cast<uintptr_t>(help.layer) : 0;

  const bool over = hit.hit != nullptr;
  if (pressed && over)
  {
    m_captured = true;
    const bool holds = (hit.hit->action || hit.hit->drag) && hit.hit->enabled;
    m_pressedLayer = holds ? hit.layer : nullptr;
    m_pressed = holds ? hit.hit->id : 0;
    if (hit.hit->press && hit.hit->enabled)
    {
      const std::function<void(glm::vec2)> press = hit.hit->press;
      press(m_position);
    }
  }
  // The wheel: the innermost scroll view under the pointer that can still go
  // that way.
  if (inView && (wheel.x != 0.0f || wheel.y != 0.0f))
  {
    const glm::vec2 delta = glm::vec2(wheel.x, -wheel.y) * kWheelStep * m_scale;
    bool done = false;
    for (auto layer = m_drawn.rbegin(); layer != m_drawn.rend() && !done;
         ++layer)
    {
      const std::vector<ui::Hit>& hits = layer->second->cache.hits();
      for (auto each = hits.rbegin(); each != hits.rend(); ++each)
        if (each->scroll && each->rect.contains(m_position))
        {
          const std::function<bool(glm::vec2)> scroll = each->scroll;
          if (scroll(delta))
          {
            done = true;
            break;
          }
        }
    }
  }
  // A slider held down follows the pointer, wherever it goes.
  if (m_pressed != 0 && (pressed || held))
    for (const auto& [layer, kept] : m_drawn)
      if (layer == m_pressedLayer)
        for (const ui::Hit& each : kept->cache.hits())
          if (each.id == m_pressed && each.drag)
          {
            // (Copied: it may change the game, and with it the UI.)
            const std::function<void(const ui::Node&, glm::vec2)> drag =
                each.drag;
            const ui::Node* source = sourceOf({layer, &each});
            if (source)
              drag(*source, m_position);
            break;
          }
  if (released && m_pressed != 0)
  {
    // A click: let go over the button pressed.
    if (hit.hit && hit.layer == m_pressedLayer && hit.hit->id == m_pressed &&
        hit.hit->action && hit.hit->enabled)
    {
      const std::function<void(const ui::Node&)> action = hit.hit->action;
      if (const ui::Node* source = sourceOf(hit))
        action(*source);
    }
    m_pressedLayer = nullptr;
    m_pressed = 0;
  }

  const bool theUis = m_captured || (over && !held);
  if (released || !held)
    m_captured = false;
  return theUis;
}

} // namespace sfs
