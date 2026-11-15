#include "../../testHarness.h"

#include <engine/core/assets/assetDatabase.h>
#include <engine/core/reflection/registration.h>
#include <engine/runtime/scriptableObjects/scriptableObjects.h>
#include <engine/runtime/serialization/sceneSerializer.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
  AssetRefList<ItemDef> parts;

  static inline int alive = 0; // live objects, to see them freed
  ItemDef() { ++alive; }
  ~ItemDef() override { --alive; }
};

struct OtherDef : ScriptableObject
{
};

// A type derived from another reflected one (as a game's TractorSpec from
// the engine's VehicleSpec), registered as generated code would.
struct MagicItemDef : ItemDef
{
  float charge = 1.5f;
};

// One whose base is never registered.
struct Orphan : ScriptableObject
{
  int own = 3;
};

reflect::FieldInfo
fieldOf(const char* name, reflect::FieldKind kind, void* (*address)(void*))
{
  reflect::FieldInfo field;
  field.name = name;
  field.kind = kind;
  field.address = address;
  return field;
}

void registerDerived()
{
  using namespace reflect;
  TypeInfo magic = derivedType<MagicItemDef>(
      "MagicItemDef", "Magic Item Def", "derived", "ItemDef");
  magic.fields.push_back(fieldOf(
      "charge",
      FieldKind::Float,
      [](void* o) -> void* { return &objectAs<MagicItemDef>(o)->charge; }));
  types().add(magic);
}

void registerTypes()
{
  using namespace reflect;
  TypeInfo item = scriptableObjectType<ItemDef>("ItemDef", "Item Def", "test");
  const auto add =
      [&](const char* name, FieldKind kind, void* (*address)(void*))
  {
    FieldInfo field;
    field.name = name;
    field.kind = kind;
    field.address = address;
    item.fields.push_back(field);
  };
  add("displayName",
      FieldKind::String,
      [](void* o) -> void* { return &objectAs<ItemDef>(o)->displayName; });
  add("value",
      FieldKind::Int,
      [](void* o) -> void* { return &objectAs<ItemDef>(o)->value; });
  add("upgradesTo",
      FieldKind::AssetRef,
      [](void* o) -> void* { return &objectAs<ItemDef>(o)->upgradesTo; });
  item.fields.back().attributes.assetType = "ItemDef";
  add("parts",
      FieldKind::AssetRefList,
      [](void* o) -> void* { return &objectAs<ItemDef>(o)->parts; });
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

  void write(const std::string& name,
             const std::string& type,
             const nlohmann::json& fields)
  {
    std::ofstream(root / "assets/items" / (name + ".asset"))
        << ScriptableObjects::fileContents(type, fields).dump(2);
  }
};

std::string refOf(const AssetDatabase& catalogue, const std::string& name)
{
  return "asset:" +
         catalogue.findByPath("assets/items/" + name + ".asset")->id.toString();
}
} // namespace

int main()
{
  registerTypes();

  TEST("a scriptable object should load from its file, shared, with refs to "
       "others")
  {
    Project project;
    project.write(
        "Sword", "ItemDef", {{"displayName", "Sword"}, {"value", 25}});
    AssetDatabase catalogue(
        project.root, AssetDatabase::Mode::ReadWrite, {"assets"});
    const std::string sword = refOf(catalogue, "Sword");
    project.write("Dagger",
                  "ItemDef",
                  {{"displayName", "Dagger"}, {"upgradesTo", sword}});
    catalogue.refresh();

    ScriptableObjects assets;
    assets.setCatalogue(&catalogue);
    CHECK(assets.list("ItemDef").size() == 2u);
    CHECK(assets.list("OtherDef").empty());

    const ItemDef* dagger = assets.get<ItemDef>(refOf(catalogue, "Dagger"));
    CHECK(dagger && dagger->displayName == "Dagger" &&
          dagger->value == 10); // value: its default
    CHECK(dagger->upgradesTo.asset == sword);
    CHECK(assets.get<ItemDef>(sword) ==
          assets.get<ItemDef>(sword));             // one object, shared
    CHECK(assets.get<OtherDef>(sword) == nullptr); // not that type
    CHECK(assets.get("asset:00000000000000000000000000000001") == nullptr);

    // All of a type, by name.
    const auto all = assets.all<ItemDef>();
    CHECK(all.size() == 2u && all[0].name == "Dagger" &&
          all[1].data->value == 25);

    // Forgotten, it's read again (its file changed); cleared, all are freed.
    project.write(
        "Sword", "ItemDef", {{"displayName", "Sword"}, {"value", 40}});
    assets.forget(sword);
    CHECK(assets.get<ItemDef>(sword)->value == 40);
    assets.clear();
    CHECK(ItemDef::alive == 0);
  }

  TEST("AssetRef should get its asset through the shared scriptable objects")
  {
    Project project;
    project.write("Shield", "ItemDef", {{"value", 7}});
    AssetDatabase catalogue(
        project.root, AssetDatabase::Mode::ReadWrite, {"assets"});
    scriptableObjects().setCatalogue(&catalogue);

    AssetRef<ItemDef> ref;
    CHECK(!ref);
    ref.asset = refOf(catalogue, "Shield");
    CHECK(ref && ref->value == 7);
    scriptableObjects().setCatalogue(nullptr);
    CHECK(ItemDef::alive == 0);
  }

  TEST("AssetRefList should load, save and get its assets in order")
  {
    Project project;
    project.write("Blade", "ItemDef", {{"value", 3}});
    project.write("Hilt", "ItemDef", {{"value", 1}});
    AssetDatabase catalogue(
        project.root, AssetDatabase::Mode::ReadWrite, {"assets"});
    const std::string blade = refOf(catalogue, "Blade");
    const std::string hilt = refOf(catalogue, "Hilt");
    const std::string gone = "asset:00000000000000000000000000000001";
    project.write("Sword",
                  "ItemDef",
                  {{"parts", nlohmann::json::array({hilt, "", gone, blade})}});
    catalogue.refresh();
    scriptableObjects().setCatalogue(&catalogue);

    const auto* sword =
        scriptableObjects().get<ItemDef>(refOf(catalogue, "Sword"));
    CHECK(sword && sword->parts.size() == 4u);
    CHECK(sword->parts.get(0) && sword->parts.get(0)->value == 1);
    CHECK(sword->parts.get(1) == nullptr); // unset
    CHECK(sword->parts.get(2) == nullptr); // missing
    CHECK(sword->parts.get(9) == nullptr); // past the end
    const std::vector<const ItemDef*> all = sword->parts.all();
    CHECK(all.size() == 2u && all[0]->value == 1 && all[1]->value == 3);

    // Saved as an array of its assets, unset and missing ones kept; loaded
    // back, the same.
    const reflect::TypeInfo& type = *reflect::types().find("ItemDef");
    const SceneSerializer serializer(reflect::types(), nullptr);
    const nlohmann::json saved =
        serializer.saveObject(type, const_cast<ItemDef*>(sword));
    CHECK(saved["parts"] == nlohmann::json::array({hilt, "", gone, blade}));
    ItemDef copy;
    serializer.loadObject(type, &copy, saved);
    CHECK(copy.parts == sword->parts);

    // Not an array of strings: left as it was.
    ItemDef bad;
    bad.parts.assets = {blade};
    serializer.loadObject(type, &bad, {{"parts", 3}});
    CHECK(bad.parts.size() == 1u);
    scriptableObjects().setCatalogue(nullptr);
  }

  TEST("a derived type should have its bases' fields first, then its own, "
       "and know what it is")
  {
    registerDerived();
    const reflect::TypeInfo* magic = reflect::types().find("MagicItemDef");
    CHECK(magic && magic->kind == reflect::TypeKind::ScriptableObject);
    CHECK(magic && magic->fields.size() == 5u);
    CHECK(magic && magic->fields[0].name == "displayName" &&
          magic->fields[0].owner == "ItemDef");
    CHECK(magic && magic->fields[4].name == "charge" &&
          magic->fields[4].owner == "MagicItemDef");
    CHECK(reflect::types().isA("MagicItemDef", "ItemDef"));
    CHECK(reflect::types().isA("MagicItemDef", "MagicItemDef"));
    CHECK(!reflect::types().isA("ItemDef", "MagicItemDef"));
    CHECK(reflect::types().chain("MagicItemDef") ==
          std::vector<std::string>({"MagicItemDef", "ItemDef"}));
    // The base's own list is untouched.
    CHECK(reflect::types().find("ItemDef")->fields.size() == 4u);
  }

  TEST("a derived asset should load and save every level's fields, and an "
       "AssetRef to its base should take it")
  {
    registerDerived();
    Project project;
    project.write("Wand",
                  "MagicItemDef",
                  {{"displayName", "Wand"}, {"value", 30}, {"charge", 4.0}});
    project.write("Stick", "ItemDef", {{"value", 2}});
    AssetDatabase catalogue(
        project.root, AssetDatabase::Mode::ReadWrite, {"assets"});
    scriptableObjects().setCatalogue(&catalogue);
    const std::string wand = refOf(catalogue, "Wand");

    const auto* magic = scriptableObjects().get<MagicItemDef>(wand);
    CHECK(magic && magic->displayName == "Wand" && magic->value == 30 &&
          magic->charge == 4.0f);
    AssetRef<ItemDef> ref;
    ref.asset = wand;
    CHECK(ref && ref->value == 30 && ref.get() == magic);

    // Round trip: saved, every level's fields; loaded back, the same.
    const reflect::TypeInfo& type = *reflect::types().find("MagicItemDef");
    const SceneSerializer serializer(reflect::types(), nullptr);
    const nlohmann::json saved =
        serializer.saveObject(type, const_cast<MagicItemDef*>(magic));
    CHECK(saved.value("value", 0) == 30 &&
          saved.value("charge", 0.0f) == 4.0f &&
          saved.value("displayName", std::string()) == "Wand");
    std::unique_ptr<void, void (*)(void*)> copy(
        type.scriptable.create(), type.scriptable.destroy);
    serializer.loadObject(type, copy.get(), saved);
    const auto* back = dynamic_cast<const MagicItemDef*>(
        static_cast<ScriptableObject*>(copy.get()));
    CHECK(back && back->value == 30 && back->charge == 4.0f &&
          back->displayName == "Wand");

    // The picker's list for the base has both; the derived one's, its own.
    const auto bases = scriptableObjects().list("ItemDef");
    CHECK(bases.size() == 2u);
    const auto derived = scriptableObjects().list("MagicItemDef");
    CHECK(derived.size() == 1u && derived[0].asset == wand);
    CHECK(scriptableObjects().get<MagicItemDef>(refOf(catalogue, "Stick")) ==
          nullptr);
    scriptableObjects().setCatalogue(nullptr);
  }

  TEST("a type whose base is unknown or removed should keep its own fields, "
       "and get the base's when it comes")
  {
    using namespace reflect;
    TypeInfo orphan =
        derivedType<Orphan>("Orphan", "Orphan", "orphans", "LaterBase");
    orphan.fields.push_back(fieldOf("own",
                                    FieldKind::Int,
                                    [](void* o) -> void*
                                    { return &objectAs<Orphan>(o)->own; }));
    types().add(orphan);
    CHECK(types().find("Orphan")->fields.size() == 1u);
    CHECK(!types().isA("Orphan", "LaterBase"));

    // (Shaped like one: a base with one field, from another module.)
    TypeInfo base =
        scriptableObjectType<OtherDef>("LaterBase", "Later Base", "bases");
    base.fields.push_back(
        fieldOf("extra", FieldKind::Int, [](void* o) -> void* { return o; }));
    types().add(base);
    CHECK(types().find("Orphan")->fields.size() == 2u);
    CHECK(types().find("Orphan")->fields[0].name == "extra");
    CHECK(types().isA("Orphan", "LaterBase"));

    types().removeModule("bases");
    CHECK(types().find("Orphan")->fields.size() == 1u);
    CHECK(types().find("Orphan")->fields[0].name == "own");
    types().removeModule("orphans");
  }

  return testing::report("scriptableObjectsTests");
}
