#include "../../engine/tests/runtime/rendering/mockRenderer.h"
#include "../../engine/tests/testHarness.h"
#include "partsTestSupport.h"

#include "editorContext.h"
#include "history/editorHistory.h"
#include "scene/editorScene.h"
#include "scene/prefabs.h"
#include "scene/sceneDocument.h"
#include "scene/unusedOverrides.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/sceneManager/sceneManager.h"
#include "engine/runtime/serialization/prefabComponents.h"

#include <memory>

using namespace partsTests;

namespace
{
constexpr const char* kRigId = "a123456789abcdef0123456789abcdef";
constexpr const char* kYardId = "b123456789abcdef0123456789abcdef";
const std::string kRig = std::string("asset:") + kRigId;
const std::string kYard = std::string("asset:") + kYardId;

// A project with a rig prefab (its arm and blades its parts) and a yard
// holding a rig as its entity 5, with a post (3) whose arm is the rig's.
fs::path writeProject()
{
  const fs::path project = fs::temp_directory_path() / "sfs-prefabs-test";
  fs::remove_all(project);
  writeFile(project / "prefabs" / "Rig.prefab",
            rigPrefab({{"arm", "1"}, {"blades", {"2", "3"}}}));
  writeMeta(project / "prefabs" / "Rig.prefab", kRigId);
  json yard = named("Yard");
  yard["children"] = json::array(
      {{{"$id", 5}, {"$prefab", kRig}},
       named("Post",
             {{"$id", 3},
              {"PartsComponent", {{"arm", "5/1"}, {"blades", {}}}}})});
  writeFile(project / "prefabs" / "Yard.prefab",
            {{"formatVersion", 2}, {"root", yard}});
  writeMeta(project / "prefabs" / "Yard.prefab", kYardId);
  return project;
}

// The editor's side of one scene: its document, history and context.
struct Editor
{
  explicit Editor(const fs::path& project)
      : database(project,
                 sfs::AssetDatabase::Mode::ReadOnly,
                 sfs::AssetDatabase::projectFolders())
  {
    store.setDatabase(&database);
    manager.setSceneServices(&services);
    scene = manager.createScene<EditorScene>();
    document = std::make_unique<SceneDocument>(*scene);
    history = std::make_unique<EditorHistory>(context);
    context.scene = scene;
    context.render = &scene->renderSystem();
    context.document = document.get();
    context.history = history.get();
    document->restore(json{{"formatVersion", 3}, {"entities", json::array()}});
  }

  sfs::AssetDatabase database;
  sfs::AssetStore store;
  sfs::testing3d::MockRenderer renderer;
  sfs::SceneServices services{.assetStore = store, .renderer = &renderer};
  sfs::SceneManager manager;
  EditorScene* scene = nullptr;
  std::unique_ptr<SceneDocument> document;
  std::unique_ptr<EditorHistory> history;
  EditorContext context;

  Entity place(const std::string& prefab, const Entity& parent = {})
  {
    const Entity root = scene->serializer().instantiate(prefab, *scene, parent);
    scene->flushEntities();
    return root;
  }

  Entity make(const std::string& name)
  {
    Entity entity = scene->createEmpty(name);
    scene->flushEntities();
    return entity;
  }

  Entity find(const std::string& name)
  {
    for (const Entity& entity : scene->entities())
      if (entityLabel(entity) == name)
        return entity;
    return {};
  }
};
} // namespace

int main()
{
  registerParts();
  const fs::path project = writeProject();

  TEST("reverting an instance should keep the references other entities "
       "hold into it, and its own references out of it")
  {
    Editor editor(project);
    Entity rig = editor.place(kRig);
    Entity lamp = editor.make("Lamp");
    lamp.addComponent<PartsComponent>(
        PartsComponent{childNamed(rig, "Arm"), {rig}});
    // The rig's own arm overridden to point out of it, at the lamp.
    rig.getComponent<PartsComponent>()->arm = lamp;
    rig.getComponent<sfs::NameComponent>()->name = "Renamed";
    editor.scene->flushEntities();

    prefabs::revertField(editor.context, rig, "NameComponent", "name");
    const Entity again = editor.find("Rig");
    CHECK(again.isValid() && again != rig);
    CHECK(lamp.getComponent<PartsComponent>()->arm == childNamed(again, "Arm"));
    CHECK(lamp.getComponent<PartsComponent>()->blades ==
          std::vector<Entity>({again}));
    CHECK(again.getComponent<PartsComponent>()->arm == lamp);

    // A whole revert drops the instance's own reference, keeps the lamp's.
    prefabs::revert(editor.context, again);
    const Entity third = editor.find("Rig");
    CHECK(lamp.getComponent<PartsComponent>()->arm == childNamed(third, "Arm"));
    CHECK(third.getComponent<PartsComponent>()->arm ==
          childNamed(third, "Arm"));

    // Undone, the override is back.
    editor.history->undo();
    const Entity undone = editor.find("Rig");
    CHECK(undone.getComponent<PartsComponent>()->arm == editor.find("Lamp"));
  }

  TEST("in a prefab's tab, reverting a nested instance should keep its id in "
       "the prefab, and the prefab's references into it")
  {
    Editor editor(project);
    editor.context.editingPrefab = true;
    editor.document->setPrefabContent(true);
    Entity yard = editor.place(kYard);
    yard.removeComponent<sfs::PrefabInstanceComponent>();
    const Entity rig = childNamed(yard, "Rig");
    const Entity post = childNamed(yard, "Post");
    CHECK(post.getComponent<PartsComponent>()->arm == childNamed(rig, "Arm"));
    const json before = editor.scene->serializer().savePrefab(yard, true);

    prefabs::revert(editor.context, rig);
    const Entity again = childNamed(yard, "Rig");
    CHECK(again.isValid() && again != rig);
    const auto* againLink = again.getComponent<sfs::PrefabLinkComponent>();
    CHECK((againLink && againLink->id == 5u));
    CHECK(post.getComponent<PartsComponent>()->arm == childNamed(again, "Arm"));
    CHECK(editor.scene->serializer().savePrefab(yard, true) == before);

    // Undo and redo keep the prefab's ids too (its snapshots save them).
    editor.history->undo();
    editor.history->redo();
    const Entity yardAgain = editor.find("Yard");
    CHECK(editor.scene->serializer().savePrefab(yardAgain, true) == before);
  }

  TEST("an unused override should be reattached, or removed, as an undoable "
       "edit")
  {
    Editor editor(project);
    const json entry = {{"$prefab", kRig},
                        {"$overrides", {{"99", named("Elbow")}}},
                        {"$removed", {"98"}}};
    editor.scene->serializer().loadEntity(entry, *editor.scene);
    editor.scene->flushEntities();
    Entity rig = editor.find("Rig");
    CHECK(unusedOverrides::of(rig).size() == 2u);

    prefabs::reattachUnused(editor.context, rig, {{"99", "1"}});
    rig = editor.context.selection;
    CHECK(rig == editor.find("Rig"));
    CHECK(childNamed(rig, "Elbow").isValid());
    const std::vector<unusedOverrides::Unused> left = unusedOverrides::of(rig);
    CHECK(left.size() == 1u &&
          left[0].kind == unusedOverrides::Unused::Kind::Removed);

    if (!left.empty())
      prefabs::removeUnused(editor.context, rig, left[0]);
    rig = editor.context.selection;
    CHECK(unusedOverrides::of(rig).empty());
    CHECK(!editor.scene->serializer().instanceEntry(rig).contains("$removed"));

    editor.history->undo();
    CHECK(unusedOverrides::of(editor.find("Rig")).size() == 1u);
    editor.history->undo();
    CHECK(unusedOverrides::of(editor.find("Rig")).size() == 2u);
  }

  fs::remove_all(project);
  return testing::report("prefabsTests");
}
