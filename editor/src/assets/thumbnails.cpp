#include "assets/thumbnails.h"

#include "engine/core/rendering/3d/meshPreview.h"
#include "scene/editorScene.h"

#include "engine/core/components/transform3DComponent.h"
#include "engine/core/jobs/workerPool.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelImporter.h"
#include "engine/runtime/assetStore/textureImporter.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "engine/webgpu/webGpuContext.h"

#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

#include <cstdint>
#include <string_view>

namespace
{
// Gathering what a preview needs (reading a material, expanding a prefab)
// happens on the main thread, so only this many start a frame.
constexpr int kPerFrame = 4;

glm::vec3 vec3Of(const nlohmann::json& json, const char* key, glm::vec3 fallback)
{
  const auto it = json.find(key);
  if (it == json.end() || !it->is_array() || it->size() != 3)
    return fallback;
  return {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}

// An entity's local transform, from its scene-file components.
glm::mat4 localMatrix(const nlohmann::json& components)
{
  const auto it = components.find("Transform3DComponent");
  if (it == components.end())
    return glm::mat4(1.0f);
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  if (const auto r = it->find("rotation"); r != it->end() && r->is_array() && r->size() == 4)
    rotation = glm::quat((*r)[0].get<float>(), (*r)[1].get<float>(), (*r)[2].get<float>(), (*r)[3].get<float>());
  return sfs::Transform3DComponent(vec3Of(*it, "position", glm::vec3(0.0f)), rotation, vec3Of(*it, "scale", glm::vec3(1.0f)))
      .matrix();
}

// A material slot's choice ("asset:<id>") as a material; null for the mesh's own.
std::shared_ptr<const sfs::MeshMaterial> slotMaterial(sfs::AssetStore& store, const std::string& name)
{
  constexpr std::string_view prefix = "asset:";
  if (!name.starts_with(prefix))
    return nullptr;
  const std::optional<sfs::AssetId> id = sfs::AssetId::parse(std::string_view(name).substr(prefix.size()));
  return id ? store.material(*id) : nullptr;
}

// Add `part` to `out`, placed by `world`, tinted, with its slots' materials.
void append(sfs::MeshData& out,
            const sfs::MeshData& part,
            const glm::mat4& world,
            const glm::vec4& tint,
            const std::vector<std::shared_ptr<const sfs::MeshMaterial>>& slots)
{
  const uint32_t base = static_cast<uint32_t>(out.vertices.size());
  const uint32_t firstIndex = static_cast<uint32_t>(out.indices.size());
  const glm::mat3 normals = glm::transpose(glm::inverse(glm::mat3(world)));
  for (sfs::MeshVertex vertex : part.vertices)
  {
    vertex.position = glm::vec3(world * glm::vec4(vertex.position, 1.0f));
    vertex.normal = glm::normalize(normals * vertex.normal);
    vertex.color *= glm::vec3(tint);
    out.vertices.push_back(vertex);
  }
  for (const uint32_t index : part.indices)
    out.indices.push_back(base + index);

  std::vector<sfs::MeshSection> sections = part.sections;
  if (sections.empty())
    sections.push_back({0, static_cast<uint32_t>(part.indices.size()), {}, {}});
  for (std::size_t slot = 0; slot < sections.size(); ++slot)
  {
    sfs::MeshSection section = sections[slot];
    section.firstIndex += firstIndex;
    if (slot < slots.size() && slots[slot])
      section.material = *slots[slot];
    out.sections.push_back(std::move(section));
  }
}

// A prefab's meshes as one, in place: what its preview draws. Nothing while
// any of them is still loading.
std::optional<sfs::MeshData> prefabMesh(const sfs::AssetId& prefab, EditorScene& scene, sfs::AssetStore& store)
{
  sfs::MeshLibrary& meshes = scene.renderSystem().meshes();
  std::unordered_map<std::string, glm::mat4> world;
  sfs::MeshData combined;
  for (const auto& entity : scene.serializer().expandPrefab("asset:" + prefab.toString()))
  {
    const auto parent = world.find(entity.parent);
    const glm::mat4 placed = (entity.key.empty() || parent == world.end() ? glm::mat4(1.0f) : parent->second) *
                             localMatrix(entity.components);
    world[entity.key] = placed;

    const auto renderer = entity.components.find("MeshRendererComponent");
    if (renderer == entity.components.end() || !renderer->value("mesh", nlohmann::json()).is_string())
      continue;
    const sfs::MeshHandle mesh = meshes.resolve((*renderer)["mesh"].get<std::string>());
    if (meshes.isPending(mesh))
      return std::nullopt;
    const std::optional<sfs::MeshData> data = meshes.meshData(mesh);
    if (!data)
      continue;
    glm::vec4 tint(1.0f);
    if (const auto t = renderer->find("tint"); t != renderer->end() && t->is_array() && t->size() == 4)
      tint = {(*t)[0].get<float>(), (*t)[1].get<float>(), (*t)[2].get<float>(), (*t)[3].get<float>()};
    std::vector<std::shared_ptr<const sfs::MeshMaterial>> slots;
    if (const auto m = renderer->find("materials"); m != renderer->end() && m->is_array())
      for (const auto& name : *m)
        slots.push_back(name.is_string() ? slotMaterial(store, name.get<std::string>()) : nullptr);
    append(combined, *data, placed, tint, slots);
  }
  return combined;
}
} // namespace

Thumbnails::Thumbnails(sfs::WebGpuContext& context)
    : m_context(context), m_workers(std::make_unique<sfs::WorkerPool>(2))
{
}

Thumbnails::~Thumbnails() = default;

void Thumbnails::beginFrame()
{
  m_budget = kPerFrame;

  std::vector<Finished> finished;
  {
    const std::lock_guard lock(m_finishedMutex);
    finished.swap(m_finished);
  }
  for (Finished& done : finished)
  {
    m_progress.done = std::min(m_progress.done + 1, m_progress.total);
    const auto it = m_entries.find(done.id);
    if (it == m_entries.end() || it->second.ticket != done.ticket)
      continue; // forgotten, or superseded by a newer one
    Entry& entry = it->second;
    entry.ticket = 0;
    entry.view.reset();
    entry.texture.reset();
    if (!done.rgba.empty())
      upload(entry, done.rgba);
  }
}

ImTextureID Thumbnails::get(const sfs::AssetRecord& record,
                            const sfs::AssetDatabase& assets,
                            sfs::AssetStore& store,
                            EditorScene* scene)
{
  if (record.kind != sfs::AssetKind::Texture && record.kind != sfs::AssetKind::Material &&
      record.kind != sfs::AssetKind::Model && record.kind != sfs::AssetKind::Prefab)
    return ImTextureID{};

  const auto shown = [](const Entry& entry)
  { return entry.view ? static_cast<ImTextureID>(reinterpret_cast<intptr_t>(entry.view.get())) : ImTextureID{}; };

  auto it = m_entries.find(record.id);
  if (it != m_entries.end() && it->second.checked == m_generation)
    return shown(it->second);

  std::error_code error;
  const auto stamp = std::filesystem::last_write_time(assets.absolutePath(record), error);
  if (it != m_entries.end() && it->second.stamp == stamp)
  {
    it->second.checked = m_generation;
    return shown(it->second);
  }

  // New, or changed since it was made: start making it, if this frame has
  // room. The old one (if any) shows meanwhile.
  if (m_budget <= 0)
    return it != m_entries.end() ? shown(it->second) : ImTextureID{};
  --m_budget;
  std::optional<Job> job = makeJob(record, assets, store, scene);
  if (!job)
    return it != m_entries.end() ? shown(it->second) : ImTextureID{}; // try again later

  Entry& entry = m_entries[record.id];
  entry.stamp = stamp;
  entry.checked = m_generation;
  entry.ticket = m_nextTicket++;
  if (m_progress.done == m_progress.total)
    m_progress = {};
  ++m_progress.total;
  m_workers->submit(
      [this, id = record.id, ticket = entry.ticket, job = std::move(*job)]
      {
        std::vector<uint8_t> rgba = job();
        const std::lock_guard lock(m_finishedMutex);
        m_finished.push_back({id, ticket, std::move(rgba)});
      });
  return shown(entry);
}

std::optional<Thumbnails::Job> Thumbnails::makeJob(const sfs::AssetRecord& record,
                                                   const sfs::AssetDatabase& assets,
                                                   sfs::AssetStore& store,
                                                   EditorScene* scene)
{
  // Files are read on the worker where they can be (not through the store,
  // which belongs to the main thread); the rest is gathered here.
  const std::filesystem::path file = assets.absolutePath(record);
  switch (record.kind)
  {
  case sfs::AssetKind::Texture:
    return [file, settings = sfs::TextureImportSettings::fromJson(record.settings)]
    {
      const auto texture = sfs::importTexture(file, settings);
      return texture ? sfs::meshPreview::fitTexture(*texture, kSize) : std::vector<uint8_t>();
    };
  case sfs::AssetKind::Model:
    return [file, settings = sfs::ModelImportSettings::fromJson(record.settings)]
    {
      const std::optional<sfs::ModelData> model = sfs::importModel(file, settings);
      return model ? sfs::meshPreview::render(model->whole, kSize) : std::vector<uint8_t>();
    };
  case sfs::AssetKind::Material:
  {
    store.forgetMaterial(record.id); // its file changed: read it again
    return [material = store.material(record.id)]
    { return material ? sfs::meshPreview::renderMaterial(*material, kSize) : std::vector<uint8_t>(); };
  }
  case sfs::AssetKind::Prefab:
  {
    if (!scene)
      return std::nullopt;
    store.forgetPrefab(record.id); // its file may have changed
    std::optional<sfs::MeshData> mesh = prefabMesh(record.id, *scene, store);
    if (!mesh)
      return std::nullopt;
    return [mesh = std::move(*mesh)]
    { return mesh.empty() ? std::vector<uint8_t>() : sfs::meshPreview::render(mesh, kSize); };
  }
  default:
    return std::nullopt;
  }
}

bool Thumbnails::upload(Entry& entry, const std::vector<uint8_t>& rgba)
{
  // The texels as stored (sRGB) in a plain UNORM texture: ImGui shows it
  // as-is, like the viewport's image.
  WGPUTextureDescriptor desc = {};
  desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  desc.dimension = WGPUTextureDimension_2D;
  desc.size = {kSize, kSize, 1};
  desc.format = WGPUTextureFormat_RGBA8Unorm;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  entry.texture.reset(wgpuDeviceCreateTexture(m_context.device(), &desc));
  if (!entry.texture)
    return false;

  WGPUTexelCopyTextureInfo destination = {};
  destination.texture = entry.texture.get();
  destination.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferLayout layout = {};
  layout.bytesPerRow = kSize * 4;
  layout.rowsPerImage = kSize;
  const WGPUExtent3D size = {kSize, kSize, 1};
  wgpuQueueWriteTexture(m_context.queue(), &destination, rgba.data(), rgba.size(), &layout, &size);

  entry.view.reset(wgpuTextureCreateView(entry.texture.get(), nullptr));
  return static_cast<bool>(entry.view);
}
