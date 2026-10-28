#include "../../engine/tests/testHarness.h"

#include "scripting/fieldMigration.h"

using namespace fieldMigration;
using nlohmann::json;
using sfs::reflect::FieldKind;
using sfs::reflect::TypeKind;

namespace
{
Layout script(const std::string& name, std::vector<Field> fields)
{
  return {name, TypeKind::GameObject, std::move(fields)};
}

Layout component(const std::string& name, std::vector<Field> fields)
{
  return {name, TypeKind::Component, std::move(fields)};
}

// A scene with one Spin script on an entity.
json spinScene(json fields)
{
  return {{"entities", json::array({{{"ScriptComponent", {{"script", "Spin"}, {"fields", fields}}}}})}};
}
} // namespace

int main()
{
  TEST("a field renamed in place should be a rename, and its saved values follow it")
  {
    const Changes changes = diff({script("Spin", {{"axis", FieldKind::Vec3}, {"degreesPerSecond", FieldKind::Float}})},
                                 {script("Spin", {{"axis", FieldKind::Vec3}, {"speed", FieldKind::Float}})});
    CHECK(changes.scripts.at("Spin").renamed.at("degreesPerSecond") == "speed");
    CHECK(changes.scripts.at("Spin").removed.empty());
    json scene = spinScene({{"axis", {0, 0, 1}}, {"degreesPerSecond", 40}});
    CHECK(apply(changes, scene));
    const json& fields = scene["entities"][0]["ScriptComponent"]["fields"];
    CHECK(fields["speed"] == 40);
    CHECK(!fields.contains("degreesPerSecond"));
    CHECK(fields["axis"] == json({0, 0, 1}));
    CHECK(!apply(changes, scene)); // already done
  }

  TEST("prefab overrides, which don't name the script, should follow a rename too")
  {
    const Changes changes = diff({script("Spin", {{"degreesPerSecond", FieldKind::Float}})},
                                 {script("Spin", {{"speed", FieldKind::Float}})});
    json prefab = {{"root",
                    {{"children",
                      json::array({{{"$prefab", "asset:model"},
                                    {"$overrides", {{"3", {{"ScriptComponent", {{"fields", {{"degreesPerSecond", 40}}}}}}}}}}})}}}};
    CHECK(apply(changes, prefab));
    CHECK(prefab["root"]["children"][0]["$overrides"]["3"]["ScriptComponent"]["fields"]["speed"] == 40);
  }

  TEST("an added field should change nothing saved")
  {
    const Changes changes = diff({script("Spin", {{"speed", FieldKind::Float}})},
                                 {script("Spin", {{"speed", FieldKind::Float}, {"axis", FieldKind::Vec3}})});
    CHECK(changes.empty());
  }

  TEST("a removed field should be dropped from saved data")
  {
    const Changes changes = diff({script("Spin", {{"speed", FieldKind::Float}, {"axis", FieldKind::Vec3}})},
                                 {script("Spin", {{"speed", FieldKind::Float}})});
    CHECK(changes.scripts.at("Spin").removed.contains("axis"));
    json scene = spinScene({{"speed", 40}, {"axis", {0, 0, 1}}});
    CHECK(apply(changes, scene));
    CHECK(!scene["entities"][0]["ScriptComponent"]["fields"].contains("axis"));
    CHECK(scene["entities"][0]["ScriptComponent"]["fields"]["speed"] == 40);
  }

  TEST("a rename alongside a new field should still be found by position")
  {
    const Changes changes = diff({script("Spin", {{"axis", FieldKind::Vec3}, {"degreesPerSecond", FieldKind::Float}})},
                                 {script("Spin",
                                         {{"axis", FieldKind::Vec3}, {"speed", FieldKind::Float}, {"wobble", FieldKind::Float}})});
    CHECK(changes.scripts.at("Spin").renamed.size() == 1u);
    CHECK(changes.scripts.at("Spin").renamed.at("degreesPerSecond") == "speed");
  }

  TEST("renames among reordered fields should pair in order when kinds match")
  {
    const Changes changes = diff({script("Mover", {{"a", FieldKind::Float}, {"b", FieldKind::Bool}})},
                                 {script("Mover", {{"flag", FieldKind::Bool}, {"amount", FieldKind::Float}})});
    // Not in place, and the kinds don't line up in order: removals and additions.
    CHECK(changes.scripts.at("Mover").renamed.empty());
    CHECK(changes.scripts.at("Mover").removed.size() == 2u);

    const Changes moved = diff({script("Mover", {{"x", FieldKind::Int}, {"a", FieldKind::Float}})},
                               {script("Mover", {{"amount", FieldKind::Float}, {"x", FieldKind::Int}})});
    CHECK(moved.scripts.at("Mover").renamed.at("a") == "amount");
  }

  TEST("a field replaced by one of another kind should be a removal, not a rename")
  {
    const Changes changes = diff({script("Spin", {{"speed", FieldKind::Float}})},
                                 {script("Spin", {{"direction", FieldKind::Vec3}})});
    CHECK(changes.scripts.at("Spin").renamed.empty());
    CHECK(changes.scripts.at("Spin").removed.contains("speed"));
  }

  TEST("a component's renamed field should follow, in entities and overrides alike")
  {
    const Changes changes = diff({component("HealthComponent", {{"hp", FieldKind::Int}})},
                                 {component("HealthComponent", {{"health", FieldKind::Int}})});
    json scene = {{"entities",
                   json::array({{{"HealthComponent", {{"hp", 3}}}},
                                {{"$prefab", "asset:x"}, {"$overrides", {{"", {{"HealthComponent", {{"hp", 5}}}}}}}}})}};
    CHECK(apply(changes, scene));
    CHECK(scene["entities"][0]["HealthComponent"]["health"] == 3);
    CHECK(scene["entities"][1]["$overrides"][""]["HealthComponent"]["health"] == 5);
  }

  TEST("an override naming no script should be left alone when the rename is ambiguous")
  {
    // Both scripts renamed "speed", differently: an unnamed override can't tell which.
    const Changes changes = diff({script("A", {{"speed", FieldKind::Float}}), script("B", {{"speed", FieldKind::Float}})},
                                 {script("A", {{"pace", FieldKind::Float}}), script("B", {{"rate", FieldKind::Float}})});
    json patch = {{"ScriptComponent", {{"fields", {{"speed", 2}}}}}};
    CHECK(!apply(changes, patch));
    // Named, each follows its own.
    json scene = {{"entities", json::array({{{"ScriptComponent", {{"script", "B"}, {"fields", {{"speed", 2}}}}}}})}};
    CHECK(apply(changes, scene));
    CHECK(scene["entities"][0]["ScriptComponent"]["fields"]["rate"] == 2);
  }

  TEST("a renamed script should be followed, by the scenes and prefabs using it")
  {
    const Changes changes = diff({script("Spin", {{"axis", FieldKind::Vec3}, {"speed", FieldKind::Float}})},
                                 {script("Spinner", {{"axis", FieldKind::Vec3}, {"speed", FieldKind::Float}})});
    CHECK(changes.renamedScripts.at("Spin") == "Spinner");
    json scene = spinScene({{"speed", 40}});
    CHECK(apply(changes, scene));
    CHECK(scene["entities"][0]["ScriptComponent"]["script"] == "Spinner");
    CHECK(scene["entities"][0]["ScriptComponent"]["fields"]["speed"] == 40);
  }

  TEST("a script renamed along with a field should follow both")
  {
    // Different fields now, but the only script gone and the only one new.
    const Changes changes = diff({script("Spin", {{"degreesPerSecond", FieldKind::Float}}), script("Other", {})},
                                 {script("Spinner", {{"speed", FieldKind::Float}}), script("Other", {})});
    CHECK(changes.renamedScripts.at("Spin") == "Spinner");
    json scene = spinScene({{"degreesPerSecond", 40}});
    CHECK(apply(changes, scene));
    CHECK(scene["entities"][0]["ScriptComponent"]["script"] == "Spinner");
    CHECK(scene["entities"][0]["ScriptComponent"]["fields"]["speed"] == 40);
  }

  TEST("a renamed component should move its saved data, in overrides too")
  {
    const Changes changes = diff({component("HealthComponent", {{"hp", FieldKind::Int}})},
                                 {component("Health", {{"hp", FieldKind::Int}})});
    CHECK(changes.renamedComponents.at("HealthComponent") == "Health");
    json scene = {{"entities",
                   json::array({{{"HealthComponent", {{"hp", 3}}}},
                                {{"$prefab", "asset:x"}, {"$overrides", {{"1", {{"HealthComponent", {{"hp", 5}}}}}}}}})}};
    CHECK(apply(changes, scene));
    CHECK(scene["entities"][0]["Health"]["hp"] == 3);
    CHECK(!scene["entities"][0].contains("HealthComponent"));
    CHECK(scene["entities"][1]["$overrides"]["1"]["Health"]["hp"] == 5);
  }

  TEST("a renamed system should be renamed in a scene's systems")
  {
    const Changes changes = diff({{"GrowthSystem", TypeKind::System, {}}, {"Weather", TypeKind::System, {}}},
                                 {{"CropGrowth", TypeKind::System, {}}, {"Weather", TypeKind::System, {}}});
    CHECK(changes.renamedSystems.at("GrowthSystem") == "CropGrowth");
    json scene = {{"systems", {"GrowthSystem", "Weather"}}, {"entities", json::array()}};
    CHECK(apply(changes, scene));
    CHECK(scene["systems"] == json({"CropGrowth", "Weather"}));
  }

  TEST("types that can't be told apart should not be guessed at")
  {
    // Two scripts gone and two new, all alike: which is which is unknown.
    const Changes changes = diff({script("A", {{"x", FieldKind::Float}}), script("B", {{"x", FieldKind::Float}})},
                                 {script("C", {{"x", FieldKind::Float}}), script("D", {{"x", FieldKind::Float}})});
    CHECK(changes.renamedScripts.empty());
    // A script gone with no new one: removed, its data kept (as unresolved).
    CHECK(diff({script("Old", {})}, {}).empty());
  }

  TEST("a scriptable object type's renames should carry into its asset files")
  {
    // ItemDef renamed Item, its "cost" renamed "price" and "weight" dropped.
    const Changes changes = diff({{"ItemDef", TypeKind::ScriptableObject,
                                   {{"name", FieldKind::String}, {"cost", FieldKind::Int}, {"weight", FieldKind::Float}}}},
                                 {{"Item", TypeKind::ScriptableObject, {{"name", FieldKind::String}, {"price", FieldKind::Int}}}});
    CHECK(changes.renamedScriptableObjects.at("ItemDef") == "Item");
    json sword = {{"formatVersion", 1}, {"type", "ItemDef"}, {"fields", {{"name", "Sword"}, {"cost", 25}, {"weight", 3.0}}}};
    CHECK(applyToScriptableObject(changes, sword));
    CHECK(sword["type"] == "Item");
    CHECK(sword["fields"] == json({{"name", "Sword"}, {"price", 25}}));
    // Another type's asset: untouched.
    json other = {{"type", "Spell"}, {"fields", {{"cost", 4}}}};
    CHECK(!applyToScriptableObject(changes, other));
  }

  TEST("other scripts' fields should be untouched")
  {
    const Changes changes = diff({script("Spin", {{"speed", FieldKind::Float}})},
                                 {script("Spin", {{"pace", FieldKind::Float}})});
    json scene = {{"entities", json::array({{{"ScriptComponent", {{"script", "Other"}, {"fields", {{"speed", 2}}}}}}})}};
    CHECK(!apply(changes, scene));
  }

  return testing::report("fieldMigrationTests");
}
