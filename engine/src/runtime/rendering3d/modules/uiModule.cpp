#include "engine/runtime/rendering3d/modules/uiModule.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/logger/logger.h"
#include "engine/core/rendering/3d/meshPreview.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/runtime/ui/layer.h"

#include <SDL_filesystem.h>
#include <SDL_image.h>
#include <SDL_stdinc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace sfs
{

namespace
{
constexpr double kHelpDelay =
    0.5; // seconds over something before its help shows
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

const UiTexture* UiModule::image(const std::string& source, bool mesh)
{
  const std::string key = (mesh ? "m:" : "f:") + source;
  if (const auto it = m_images.find(key); it != m_images.end())
    return it->second.get();

  static uint32_t nextId = 0x494D0000u; // "IM": apart from fonts' textures
  auto texture = std::make_unique<UiTexture>();
  texture->id = ++nextId;
  texture->revision = 1;
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
    texture->size = {kIconSize, kIconSize};
    texture->pixels = meshPreview::render(*data, kIconSize);
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
    texture->size = {rgba->w, rgba->h};
    texture->pixels.resize(std::size_t(rgba->w) * rgba->h * 4);
    SDL_LockSurface(rgba);
    for (int y = 0; y < rgba->h; ++y)
      std::memcpy(&texture->pixels[std::size_t(y) * rgba->w * 4],
                  static_cast<const uint8_t*>(rgba->pixels) +
                      std::size_t(y) * rgba->pitch,
                  std::size_t(rgba->w) * 4);
    SDL_UnlockSurface(rgba);
    SDL_FreeSurface(rgba);
  }
  if (texture->pixels.size() !=
      std::size_t(texture->size.x) * texture->size.y * 4)
    return nullptr;
  return (m_images[key] = std::move(texture)).get();
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

void UiModule::computeCommands(const RenderContext3D& context)
{
  flush();
  m_meshes = context.meshes;
  m_screen = glm::vec2(context.viewportSize);
  m_scale = m_fixedScale > 0.0f
                ? m_fixedScale
                : std::clamp(m_screen.y / 1080.0f, 0.5f, 4.0f) * m_userScale;

  // The scene's layers (or the ones given), back to front.
  std::vector<ui::Layer*> layers;
  for (ui::Layer* layer :
       m_layerSource ? m_layerSource() : m_scene.findObjects<ui::Layer>())
    if (layer && layer->visible)
      layers.push_back(layer);
  std::stable_sort(layers.begin(),
                   layers.end(),
                   [](const ui::Layer* a, const ui::Layer* b)
                   { return a->order < b->order; });
  if (m_overlay && m_overlay->visible)
    layers.push_back(m_overlay);

  m_hits.clear();
  m_placed.clear();
  if (layers.empty() || m_screen.x <= 0.0f || m_screen.y <= 0.0f)
    return;
  auto list = std::make_shared<UiDrawList>();
  for (std::size_t i = 0; i < layers.size(); ++i)
  {
    const std::size_t firstHit = m_hits.size();
    ui::layout(layers[i]->body(),
               m_screen,
               m_scale,
               m_theme,
               m_fonts,
               m_pointer,
               list->quads,
               m_hits,
               this,
               m_recordPlaced ? &m_placed : nullptr);
    // Ids from different layers mustn't meet.
    for (std::size_t h = firstHit; h < m_hits.size(); ++h)
      m_hits[h].id = std::to_string(i) + m_hits[h].id;
  }

  // Help for what the pointer's been over a moment.
  if (const ui::Hit* help = helpAt(m_pointer.position);
      help && m_hoveredFor >= kHelpDelay && m_pointer.pressed.empty())
    ui::tooltip(help->help,
                m_pointer.position,
                m_screen,
                m_scale,
                m_theme,
                m_fonts,
                list->quads);

  if (!list->quads.empty())
    m_commands.push_back(UiDrawCommand{std::move(list)});
}

glm::vec2 UiModule::fit(const ui::View& view, glm::vec2 offer)
{
  const float scale = m_fixedScale > 0.0f ? m_fixedScale : m_scale;
  return ui::fit(view, offer, scale, m_theme, m_fonts, this);
}

const ui::Hit* UiModule::hitAt(glm::vec2 point) const
{
  // The front one: the last laid out there.
  const ui::Hit* found = nullptr;
  for (const ui::Hit& hit : m_hits)
    if (hit.rect.contains(point))
      found = &hit;
  return found;
}

const ui::Hit* UiModule::helpAt(glm::vec2 point) const
{
  // The front one with help: a button's, or a view's it's inside.
  const ui::Hit* found = nullptr;
  for (const ui::Hit& hit : m_hits)
    if (!hit.help.empty() && hit.rect.contains(point))
      found = &hit;
  return found;
}

void UiModule::handleInput(Input& input)
{
  MouseInput& mouse = input.mouse();
  // The UI's mouse: held since a press on it, or over it with nothing held
  // (a drag the game started carries on across it).
  const bool held = mouse.mouseHeld(MouseButton::Left) ||
                    mouse.mouseHeld(MouseButton::Right) ||
                    mouse.mouseHeld(MouseButton::Middle);
  if (pointer(mouse.getPosition(),
              mouse.inView(),
              mouse.mousePressed(MouseButton::Left),
              mouse.mouseReleased(MouseButton::Left),
              held))
    mouse.block();
}

bool UiModule::pointer(glm::vec2 position,
                       bool inView,
                       bool pressed,
                       bool released,
                       bool held)
{
  m_pointer.position = position;
  const ui::Hit* hit = inView ? hitAt(m_pointer.position) : nullptr;
  m_pointer.hovered = hit ? hit->id : std::string();
  // What's under the pointer for its help (timed in update()).
  const ui::Hit* help = inView ? helpAt(m_pointer.position) : nullptr;
  m_helpId = help ? help->id : std::string();

  const bool over = hit != nullptr;
  if (pressed && over)
  {
    m_captured = true;
    m_pointer.pressed = hit->action && hit->enabled ? hit->id : std::string();
  }
  if (released && !m_pointer.pressed.empty())
  {
    // A click: let go over the button pressed.
    if (hit && hit->id == m_pointer.pressed && hit->action && hit->enabled)
    {
      const std::function<void()> action = hit->action;
      action();
    }
    m_pointer.pressed.clear();
  }

  const bool theUis = m_captured || (over && !held);
  if (released || !held)
    m_captured = false;
  return theUis;
}

} // namespace sfs
