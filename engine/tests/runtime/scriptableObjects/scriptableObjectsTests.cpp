#include "../../testHarness.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/core/reflection/registration.h>
#include <engine/runtime/scriptableObjects/scriptableObjects.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace sfs;
namespace fs = std::filesystem;

namespace
{
// A scriptable object type, registered by hand as generated code would.
struct ItemDef : ScriptableObject
{
  std::string displayName;
  int value = 10;
  AssetRef<ItemDef> upgradesTo;

  static inline int alive = 0; // live objects, to see them freed
  ItemDef() { ++alive; }
  ~ItemDef() override { --alive; }
};

struct OtherDef : ScriptableObject
{
};

void registerTypes()
{
  using namespace reflect;
  TypeInfo item = scriptableObjectType<ItemDef>("ItemDef", "Item Def", "test");
  const auto add = [&](const char* name, FieldKind kind, void* (*address)(void*))
  {
    FieldInfo field;
    field.name = name;
    field.kind = kind;
    field.address = address;
    item.fields.push_back(field);
  };
  add("displayName", FieldKind::String, [](void* o) -> void* { return &static_cast<ItemDef*>(o)->displayName; });
  add("value", FieldKind::Int, [](void* o) -> void* { return &static_cast<ItemDef*>(o)->value; });
  add("upgradesTo", FieldKind::AssetRef, [](void* o) -> void* { return &static_cast<ItemDef*>(o)->upgradesTo; });
  item.fields.back().attributes.assetType = "ItemDef";
  types().add(item);
  types().add(scriptableObjectType<OtherDef>("OtherDef", "Other Def", "test"));
}

struct Project
{
  fs::path root = fs::temp_directory_path() / "sfs_data_assets";
  Project()
  {
    fs::remove_all(root);
    fs::create_directories(root / "assets/items");
  }
  ~Project() { fs::remove_all(root); }

  void write(const std::string& name, const std::string& type, const nlohmann::json& fields)
  {
    std::ofstream(root / "assets/items" / (name + ".asset")) << ScriptableObjects::fileContents(type, fields).dump(2);
  }
};

std::string refOf(const AssetDatabase& catalogue, const std::string& name)
{
  return "asset:" + catalogue.findByPath("assets/items/" + name + ".asset")->id.toString();
}
} // namespace

int main()
{
  registerTypes();

  TEST("a scriptable object should load from its file, shared, with refs to others")
  {
    Project project;
    project.write("Sword", "ItemDef", {{"displayName", "Sword"}, {"value", 25}});
    AssetDatabase catalogue(project.root, AssetDatabase::Mode::ReadWrite, {"assets"});
    const std::string sword = refOf(catalogue, "Sword");
    project.write("Dagger", "ItemDef", {{"displayName", "Dagger"}, {"upgradesTo", sword}});
    catalogue.refresh();

    ScriptableObjects assets;
    assets.setCatalogue(&catalogue);
    CHECK(assets.list("ItemDef").size() == 2u);
    CHECK(assets.list("OtherDef").empty());

    const ItemDef* dagger = assets.get<ItemDef>(refOf(catalogue, "Dagger"));
    CHECK(dagger && dagger->displayName == "Dagger" && dagger->value == 10); // value: its default
    CHECK(dagger->upgradesTo.asset == sword);
    CHECK(assets.get<ItemDef>(sword) == assets.get<ItemDef>(sword)); // one object, shared
    CHECK(assets.get<OtherDef>(sword) == nullptr);                    // not that type
    CHECK(assets.get("asset:00000000000000000000000000000001") == nullptr);

    // All of a type, by name.
    const auto all = assets.all<ItemDef>();
    CHECK(all.size() == 2u && all[0].name == "Dagger" && all[1].data->value == 25);

    // Forgotten, it's read again (its file changed); cleared, all are freed.
    project.write("Sword", "ItemDef", {{"displayName", "Sword"}, {"value", 40}});
    assets.forget(sword);
    CHECK(assets.get<ItemDef>(sword)->value == 40);
    assets.clear();
    CHECK(ItemDef::alive == 0);
  }

  TEST("AssetRef should get its asset through the shared scriptable objects")
  {
    Project project;
    project.write("Shield", "ItemDef", {{"value", 7}});
    AssetDatabase catalogue(project.root, AssetDatabase::Mode::ReadWrite, {"assets"});
    scriptableObjects().setCatalogue(&catalogue);

    AssetRef<ItemDef> ref;
    CHECK(!ref);
    ref.asset = refOf(catalogue, "Shield");
    CHECK(ref && ref->value == 7);
    scriptableObjects().setCatalogue(nullptr);
    CHECK(ItemDef::alive == 0);
  }

  return testing::report("scriptableObjectsTests");
}
