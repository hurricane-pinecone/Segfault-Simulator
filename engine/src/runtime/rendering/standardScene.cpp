#include "engine/runtime/rendering/standardScene.h"

#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/reflection/reflection.h"
#include "engine/core/rendering/meshPrimitives.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/assetStore/modelData.h"
#include "engine/runtime/rendering/modules/decalModule.h"
#include "engine/runtime/rendering/modules/grassModule.h"
#include "engine/runtime/rendering/modules/meshModule.h"
#include "engine/runtime/rendering/modules/particleModule.h"
#include "engine/runtime/rendering/modules/spriteModule.h"
#include "engine/runtime/rendering/modules/uiModule.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/scriptableObjects/scriptableObjects.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "engine/runtime/systems/physicsSystem3D.h"

namespace sfs
{

namespace
{
// Neutral grey so a primitive reads as "unstyled" until given a tint.
constexpr glm::vec3 kPrimitiveColor{0.78f, 0.78f, 0.80f};
} // namespace

PhysicsSystem3D* StandardScene::physicsSystem()
{
#ifdef SFS_HAS_PHYSICS
  return tryGetSystem<PhysicsSystem3D>();
#else
  return nullptr;
#endif
}

void StandardScene::onUpdate(double)
{
  // Models loaded in the background since: their stand-ins become them.
  AssetStore& store = assetStore();
  store.collectLoads();
  if (store.loadGeneration() != m_seenLoads)
  {
    m_seenLoads = store.loadGeneration();
    if (m_render)
      m_render->meshes().fulfilPending();
  }
}

SceneSerializer StandardScene::serializer()
{
  SceneSerializer out(reflect::types(), &m_render->meshes());
  AssetStore& store = assetStore();
  out.setPrefabSource(
      [&store](std::string_view name) -> std::optional<nlohmann::json>
      {
        const std::optional<AssetMeshName> asset = parseAssetMeshName(name);
        return asset && asset->part.empty() ? store.prefab(asset->model)
                                            : std::nullopt;
      });
  return out;
}

bool StandardScene::play(const std::filesystem::path& file)
{
  for (const Entity& entity : entities())
    destroyEntity(entity);
  flushEntities();

  if (!serializer().loadFile(file, *this, &m_gameSystems))
    return false;
  flushEntities();
  // Scriptable objects load into this scene's meshes (afresh, if another's were
  // used).
  scriptableObjects().setMeshes(&m_render->meshes());
  startScripts(*this, m_gameSystems, &m_render->meshes());
  return true;
}

Entity StandardScene::instantiate(std::string_view prefab, const Entity& parent)
{
  const Entity root = serializer().instantiate(prefab, *this, parent);
  if (root.isValid())
    startObjects(*this,
                 hierarchy::EntityTree(entities()).subtree(root),
                 &m_render->meshes());
  return root;
}

void StandardScene::filterInput(Input& input)
{
  if (UiModule* ui = m_render ? m_render->module<UiModule>() : nullptr)
    ui->handleInput(input);
}

void StandardScene::onInit()
{
  m_render = &addSystem<RenderSystem>(renderer());
  // What any style draws: meshes and grass, sprites (flat in a 2D game,
  // upright in 3D), particles and the marks they leave, and UI.
  m_render->withModule<MeshModule>();
  m_render->withModule<GrassModule>(renderer());
  m_render->withModule<SpriteModule>(assetStore());
  auto& decals = m_render->withModule<DecalModule>(assetStore());
  m_render->withModule<ParticleModule>(assetStore()).enableStains(decals);
  m_render->withModule<UiModule>(*this);

  MeshLibrary& library = m_render->meshes();
  // Named, so scenes that use them save and load.
  m_cube = library.add(
      meshes::box({1.0f, 1.0f, 1.0f}, kPrimitiveColor), "builtin:cube");
  m_sphere = library.add(
      meshes::sphere(0.5f, 24, 16, kPrimitiveColor), "builtin:sphere");
  m_plane = library.add(
      meshes::plane({10.0f, 10.0f}, kPrimitiveColor), "builtin:plane");
  m_cylinder = library.add(
      meshes::cylinder(0.5f, 1.0f, 24, kPrimitiveColor), "builtin:cylinder");

  // Meshes the game builds in code, so scenes can name them like the above.
  for (const ScriptType& script : scripts().types())
    if (script.kind == ScriptType::Kind::MeshSource && script.buildMeshes)
      script.buildMeshes(library);

  library.setProvider(modelMeshes());

  // Visual styles: .style assets, by "asset:<id>".
  AssetStore& store = assetStore();
  m_render->setStyleSource(
      [&store](std::string_view name) -> std::optional<VisualStyle>
      {
        const std::optional<AssetMeshName> asset = parseAssetMeshName(name);
        if (!asset || !asset->part.empty())
        {
          LOG_ERROR("Not a visual style asset: '" + std::string(name) + "'");
          return std::nullopt;
        }
        return store.visualStyle(asset->model);
      });
}

MeshLibrary::Provider StandardScene::modelMeshes()
{
  // Model assets, through the asset store (which imports each file once, for
  // every scene); each scene uploads its own copy of the meshes it uses.
  AssetStore& store = assetStore();
  MeshLibrary::Provider provider;

  // A model's mesh by name: the whole model, or one of its parts.
  const auto meshIn = [](const ModelData& model,
                         std::string_view part) -> const MeshData*
  {
    if (part.empty())
      return &model.whole;
    for (const ModelPart& candidate : model.parts)
      if (candidate.name == part)
        return &candidate.mesh;
    return nullptr;
  };

  // With background loading on (an editor), a model not yet loaded gives a
  // stand-in now, and its mesh once it has loaded (fulfilPending).
  provider.load = [&store, meshIn](
                      MeshLibrary& library, std::string_view name) -> MeshHandle
  {
    const std::optional<AssetMeshName> asset = parseAssetMeshName(name);
    if (!asset)
      return {};
    const ModelData* model = nullptr;
    switch (store.requestModel(asset->model, &model))
    {
    case AssetStore::LoadState::Loading:
      return library.pending(name);
    case AssetStore::LoadState::Failed:
      return {};
    case AssetStore::LoadState::Loaded:
      break;
    }
    const MeshData* mesh = meshIn(*model, asset->part);
    return mesh ? library.add(*mesh, std::string(name)) : MeshHandle{};
  };

  provider.loaded = [&store, meshIn](
                        std::string_view name, bool& waiting) -> const MeshData*
  {
    const std::optional<AssetMeshName> asset = parseAssetMeshName(name);
    if (!asset)
      return nullptr;
    const ModelData* model = nullptr;
    const AssetStore::LoadState state =
        store.requestModel(asset->model, &model);
    waiting = state == AssetStore::LoadState::Loading;
    return model ? meshIn(*model, asset->part) : nullptr;
  };

  provider.label = [&store](std::string_view name) -> std::string
  {
    const std::optional<AssetMeshName> asset = parseAssetMeshName(name);
    const AssetRecord* record = asset && store.database()
                                    ? store.database()->find(asset->model)
                                    : nullptr;
    if (!record)
      return {};
    return record->path.generic_string() +
           (asset->part.empty() ? "" : " : " + asset->part);
  };

  provider.loadMaterial =
      [&store](std::string_view name) -> std::shared_ptr<const MeshMaterial>
  {
    const std::optional<AssetMeshName> asset = parseAssetMeshName(name);
    return asset && asset->part.empty() ? store.material(asset->model)
                                        : nullptr;
  };

  // Every asset of a kind, by name and path.
  const auto assetsOf = [&store](AssetKind kind)
  {
    std::vector<std::pair<std::string, std::string>> out;
    if (store.database())
      for (const AssetRecord& record : store.database()->records())
        if (record.kind == kind && record.id.valid())
          out.emplace_back(
              assetMeshName(record.id), record.path.generic_string());
    return out;
  };
  provider.materials = [assetsOf] { return assetsOf(AssetKind::Material); };
  provider.textures = [assetsOf] { return assetsOf(AssetKind::Texture); };

  provider.available = [&store]
  {
    // Every model file, and the parts of those already loaded (listing parts
    // means reading the file, which only choosing it should cost).
    std::vector<std::pair<std::string, std::string>> out;
    if (!store.database())
      return out;
    for (const AssetRecord& record : store.database()->records())
    {
      if (record.kind != AssetKind::Model || !record.id.valid())
        continue;
      const std::string path = record.path.generic_string();
      out.emplace_back(assetMeshName(record.id), path);
      if (const ModelData* model = store.loadedModel(record.id))
        for (const ModelPart& part : model->parts)
          out.emplace_back(
              assetMeshName(record.id, part.name), path + " : " + part.name);
    }
    return out;
  };
  return provider;
}

MeshHandle StandardScene::builtinMesh(BuiltinMesh mesh) const
{
  switch (mesh)
  {
  case BuiltinMesh::Cube:
    return m_cube;
  case BuiltinMesh::Sphere:
    return m_sphere;
  case BuiltinMesh::Plane:
    return m_plane;
  case BuiltinMesh::Cylinder:
    return m_cylinder;
  }
  return {};
}

} // namespace sfs
