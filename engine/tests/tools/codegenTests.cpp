#include "../testHarness.h"

#include <sfs/codegen/attributes.h>
#include <sfs/codegen/generator.h>
#include <sfs/codegen/parser.h>
#include <sfs/codegen/rules.h>
#include <sfs/codegen/suppressions.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace sfs::codegen;

namespace
{
const FieldModel* findField(const TypeModel& type, std::string_view name)
{
  const auto it =
      std::find_if(type.fields.begin(),
                   type.fields.end(),
                   [&](const FieldModel& f) { return f.name == name; });
  return it != type.fields.end() ? &*it : nullptr;
}

bool hasDiagnostic(const ParseResult& result,
                   Severity severity,
                   std::string_view text)
{
  return std::any_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [&](const Diagnostic& d)
                     {
                       return severityOf(d) == severity &&
                              d.message.find(text) != std::string::npos;
                     });
}

// The diagnostics of `rule`.
std::vector<const Diagnostic*> ofRule(const ParseResult& result, RuleId rule)
{
  std::vector<const Diagnostic*> found;
  for (const Diagnostic& d : result.diagnostics)
    if (d.rule == rule)
      found.push_back(&d);
  return found;
}

bool hasRule(const ParseResult& result, RuleId rule, std::string_view text)
{
  for (const Diagnostic* d : ofRule(result, rule))
    if (d->message.find(text) != std::string::npos)
      return true;
  return false;
}

// `source` with a diagnostic's fix applied (edits on one line each).
std::string applyFix(std::string source, const Diagnostic& diagnostic)
{
  std::vector<TextEdit> edits = diagnostic.fix;
  std::sort(edits.begin(),
            edits.end(),
            [](const TextEdit& a, const TextEdit& b)
            {
              return std::tie(a.range.startRow, a.range.startColumn) >
                     std::tie(b.range.startRow, b.range.startColumn);
            });
  for (const TextEdit& edit : edits)
  {
    std::size_t lineStart = 0;
    for (unsigned row = 0; row < edit.range.startRow; ++row)
      lineStart = source.find('\n', lineStart) + 1;
    const std::size_t from = lineStart + edit.range.startColumn;
    const std::size_t to = lineStart + edit.range.endColumn;
    source.replace(from, to - from, edit.text);
  }
  return source;
}

std::string readText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

// The text between a docs page's begin and end markers.
std::string docsBlock(const std::filesystem::path& page, std::string_view name)
{
  const std::string text = readText(page);
  const std::string begin = "<!-- " + std::string(name) + ":begin -->\n";
  const std::string end = "<!-- " + std::string(name) + ":end -->";
  const std::size_t from = text.find(begin);
  const std::size_t to = text.find(end);
  if (from == std::string::npos || to == std::string::npos || to < from)
    return {};
  return text.substr(from + begin.size(), to - from - begin.size());
}

constexpr std::string_view kCrop = R"(
#pragma once
namespace game {
struct Helper { float notMe = 0.0f; };
struct Crop
{
  [[sfs::range(0, 1)]] float growth = 0.5f;
  [[sfs::color]] glm::vec3 tint{1.0f};
  std::string kind = "wheat";
  float a, b;
  [[sfs::hide]] int debug = 0;
  static constexpr int kMax = 4;
  const int fixed = 1;
  void grow(float dt) { growth += dt; }
  void declaredOnly();
private:
  [[sfs::show]] int secret = 3;
  float hidden;
};
}
)";

constexpr std::string_view kPlayer = R"(
class Player : public sfs::GameObject
{
public:
  float speed = 5.0f;
  Thing* target = nullptr;
  std::vector<int> items;
protected:
  void onUpdate(double dt) override {}
};
)";
} // namespace

int main()
{
  TEST("the type named after the file should be the script type")
  {
    const ParseResult result = parseHeader(kCrop, "scripts/crop.h");
    CHECK(result.type.has_value());
    CHECK(result.type->name == std::string("Crop"));
    CHECK(result.type->qualifiedName == std::string("game::Crop"));
    CHECK(result.type->kind == TypeKind::Component);
  }

  TEST("public fields, attributes and [[sfs::show]] should be collected")
  {
    const TypeModel type = *parseHeader(kCrop, "crop.h").type;

    const FieldModel* growth = findField(type, "growth");
    CHECK(growth != nullptr);
    CHECK(growth->kind == std::string("Float"));
    CHECK(growth->range.has_value());
    CHECK(growth->range->first == std::string("0"));
    CHECK(growth->range->second == std::string("1"));

    const FieldModel* tint = findField(type, "tint");
    CHECK(tint != nullptr);
    CHECK(tint->kind == std::string("Vec3"));
    CHECK(tint->color);

    CHECK(findField(type, "kind") != nullptr);
    CHECK(findField(type, "a") != nullptr); // both declarators of `float a, b;`
    CHECK(findField(type, "b") != nullptr);

    const FieldModel* secret = findField(type, "secret");
    CHECK(secret != nullptr);
    CHECK(!secret->isPublic);
  }

  TEST("hidden, private, static, const fields and methods should be skipped")
  {
    const TypeModel type = *parseHeader(kCrop, "crop.h").type;
    CHECK(findField(type, "debug") == nullptr);
    CHECK(findField(type, "hidden") == nullptr);
    CHECK(findField(type, "kMax") == nullptr);
    CHECK(findField(type, "fixed") == nullptr);
    CHECK(findField(type, "grow") == nullptr);
    CHECK(findField(type, "declaredOnly") == nullptr);
    CHECK(findField(type, "notMe") == nullptr); // other types in the header
    CHECK(type.fields.size() == 6u);
  }

  TEST("class members should default to private and bases set the kind")
  {
    const ParseResult result = parseHeader(kPlayer, "Player.h");
    CHECK(result.type->kind == TypeKind::GameObject);
    CHECK(findField(*result.type, "speed") != nullptr);
  }

  TEST("a plain class with nothing to edit should be a helper, not a component")
  {
    const ParseResult helper = parseHeader(R"(
      class FieldMap
      {
      public:
        int width() const { return m_width; }
      private:
        int m_width = 0;
      };
    )",
                                           "FieldMap.h");
    CHECK(!helper.type.has_value());
    CHECK(helper.diagnostics.empty()); // and no "no class named after the file"

    // A struct is a component even with nothing in it (a tag)...
    const ParseResult tag =
        parseHeader("struct Harvestable {};", "Harvestable.h");
    CHECK(tag.type.has_value() && tag.type->kind == TypeKind::Component);
    // ...and so is a class with public fields.
    const ParseResult withFields =
        parseHeader("class Health { public: int hp = 3; };", "Health.h");
    CHECK(withFields.type.has_value() &&
          withFields.type->kind == TypeKind::Component);
  }

  TEST("methods returning pointers or references should be skipped quietly")
  {
    const ParseResult result = parseHeader(R"(
      class Fields : public sfs::System
      {
      public:
        Thing* find(int id);
        const Thing& first() const;
        Thing** all();
      };
    )",
                                           "Fields.h");
    CHECK(result.type.has_value() && result.type->fields.empty());
    CHECK(result.diagnostics.empty());
  }

  TEST("unsupported field types should warn and be skipped")
  {
    const ParseResult result = parseHeader(kPlayer, "Player.h");
    CHECK(findField(*result.type, "target") == nullptr);
    CHECK(findField(*result.type, "items") == nullptr);
    CHECK(hasDiagnostic(result, Severity::Warning, "pointer"));
    CHECK(hasDiagnostic(result, Severity::Warning, "std::vector<int>"));
  }

  TEST("file names should match case-insensitively")
  {
    const ParseResult result = parseHeader(kPlayer, "player.h");
    CHECK(result.type.has_value());
  }

  TEST("a header without its named type should warn")
  {
    const ParseResult result = parseHeader(kPlayer, "Enemy.h");
    CHECK(!result.type.has_value());
    CHECK(hasDiagnostic(result, Severity::Warning, "Enemy"));
  }

  TEST("an unsupported base should be an error")
  {
    const ParseResult result = parseHeader(
        "class Boss : public Monster<int> { public: int hp; };", "Boss.h");
    CHECK(hasDiagnostic(result, Severity::Error, "Monster"));
    // A plain name may be another reflected type: derivedType's
    // static_assert rejects it when compiled unless it's a scriptable object.
    const ParseResult named = parseHeader(
        "class Boss : public Monster { public: int hp; };", "Boss.h");
    CHECK(named.type && named.type->kind == TypeKind::Derived &&
          named.type->base == "Monster");
  }

  TEST("systems should be recognised and nested namespaces kept")
  {
    const ParseResult result = parseHeader(
        "namespace a::b { class Growth : public sfs::System {}; }", "growth.h");
    CHECK(result.type->kind == TypeKind::System);
    CHECK(result.type->qualifiedName == std::string("a::b::Growth"));
  }

  TEST("display names should split words, acronyms and numbers")
  {
    CHECK(displayName("castsShadow", false) == std::string("Casts Shadow"));
    CHECK(displayName("MeshRendererComponent", true) ==
          std::string("Mesh Renderer"));
    CHECK(displayName("PointLight3DComponent", true) ==
          std::string("Point Light 3D"));
    CHECK(displayName("HTTPServer", false) == std::string("HTTP Server"));
    CHECK(displayName("Component", true) == std::string("Component"));
  }

  TEST("generated code should register fields and use access tags for private "
       "ones")
  {
    TypeModel type = *parseHeader(kCrop, "/project/scripts/crop.h").type;
    GeneratorOptions options;
    options.module = "game";
    options.function = "game::registerTypes";
    options.functionHeader = "game/registration.h";
    options.includeRoot = "/project";

    const std::string code = generateRegistration({type}, options);
    CHECK(code.find("#include \"scripts/crop.h\"") != std::string::npos);
    CHECK(code.find("void game::registerTypes(") != std::string::npos);
    CHECK(
        code.find("componentType<game::Crop>(\"Crop\", \"Crop\", \"game\")") !=
        std::string::npos);
    CHECK(code.find("field.attributes.hasRange = true;") != std::string::npos);
    CHECK(code.find("template struct MemberAccess<Access_game_Crop_secret, "
                    "&game::Crop::secret>;") != std::string::npos);
  }

  TEST("systems and game objects should register how to instantiate them")
  {
    TypeModel player = *parseHeader(kPlayer, "/project/scripts/Player.h").type;
    TypeModel growth = *parseHeader("class Growth : public sfs::System {};",
                                    "/project/scripts/Growth.h")
                            .type;
    GeneratorOptions options;
    options.module = "game";
    options.function = "sfsRegisterModule";
    options.includeRoot = "/project/scripts";

    const std::string code = generateRegistration({player, growth}, options);
    CHECK(code.find("#include \"engine/runtime/scripting/scriptRegistry.h\"") !=
          std::string::npos);
    CHECK(code.find("std::make_unique<Player>()") != std::string::npos);
    CHECK(code.find("scene.addSystem<Growth>()") != std::string::npos);
    CHECK(code.find("sfs::scripts().add(std::move(script));") !=
          std::string::npos);
    CHECK(
        code.find("static_assert(std::is_base_of_v<sfs::GameObject, Player>") !=
        std::string::npos);
  }

  TEST("scriptable object types and references to them should be recognised "
       "and registered")
  {
    const ParseResult item = parseHeader(R"(
      struct ItemDef : public sfs::ScriptableObject
      {
        std::string displayName;
        int value = 10;
        sfs::AssetRef<ItemDef> upgradesTo;
      };
    )",
                                         "/project/scripts/ItemDef.h");
    CHECK(item.type.has_value() &&
          item.type->kind == TypeKind::ScriptableObject);
    CHECK(item.diagnostics.empty());
    const FieldModel* upgrade = findField(*item.type, "upgradesTo");
    CHECK(upgrade && upgrade->kind == "AssetRef" &&
          upgrade->assetType == "ItemDef");

    // Spelled without sfs:: too, as a component's field.
    const ParseResult chest =
        parseHeader("struct Chest { AssetRef< ItemDef > loot; };", "Chest.h");
    const FieldModel* loot = findField(*chest.type, "loot");
    CHECK(loot && loot->kind == "AssetRef" && loot->assetType == "ItemDef");

    // A list of them.
    const ParseResult bag = parseHeader(
        "struct Bag { sfs::AssetRefList<ItemDef> items; };", "Bag.h");
    const FieldModel* items = findField(*bag.type, "items");
    CHECK(items && items->kind == "AssetRefList" &&
          items->assetType == "ItemDef");

    GeneratorOptions options;
    options.module = "game";
    options.function = "sfsRegisterModule";
    options.includeRoot = "/project/scripts";
    const std::string code = generateRegistration({*item.type}, options);
    CHECK(code.find("scriptableObjectType<ItemDef>(\"ItemDef\", \"Item Def\", "
                    "\"game\")") != std::string::npos);
    CHECK(code.find("static_assert(std::is_base_of_v<sfs::ScriptableObject, "
                    "ItemDef>") != std::string::npos);
    CHECK(code.find("sfs::AssetRef<ItemDef>>") != std::string::npos);
    CHECK(code.find("field.attributes.assetType = \"ItemDef\";") !=
          std::string::npos);
    const std::string bagCode = generateRegistration({*bag.type}, options);
    CHECK(bagCode.find("sfs::AssetRefList<ItemDef>>") != std::string::npos);
    CHECK(bagCode.find("FieldKind::AssetRefList") != std::string::npos);
    CHECK(bagCode.find("field.attributes.assetType = \"ItemDef\";") !=
          std::string::npos);
  }

  TEST("mesh sources should be recognised and register their build function")
  {
    const ParseResult result =
        parseHeader("class GameModels : public sfs::MeshSource { public: void "
                    "build(sfs::MeshLibrary& l) override; };",
                    "/project/scripts/GameModels.h");
    CHECK(result.type->kind == TypeKind::MeshSource);

    GeneratorOptions options;
    options.module = "game";
    options.function = "sfsRegisterModule";
    options.includeRoot = "/project/scripts";
    const std::string code = generateRegistration({*result.type}, options);
    CHECK(code.find("#include \"engine/runtime/rendering/meshSource.h\"") !=
          std::string::npos);
    CHECK(code.find("script.kind = sfs::ScriptType::Kind::MeshSource;") !=
          std::string::npos);
    CHECK(code.find("GameModels source;") != std::string::npos);
    CHECK(code.find("source.build(library);") != std::string::npos);
  }

  TEST("a components-only module should not depend on the runtime")
  {
    TypeModel crop = *parseHeader(kCrop, "/project/scripts/crop.h").type;
    GeneratorOptions options;
    options.module = "game";
    options.function = "registerTypes";
    const std::string code = generateRegistration({crop}, options);
    CHECK(code.find("scriptRegistry.h") == std::string::npos);
  }

  TEST("[[sfs::script]] should mark a field as a script reference")
  {
    const ParseResult result = parseHeader(
        "struct Holder { [[sfs::script]] std::string script; };", "Holder.h");
    CHECK(result.type->fields.size() == 1u);
    CHECK(result.type->fields[0].script);
  }

  TEST("[[sfs::material]] should mark a field as a material reference")
  {
    const ParseResult result = parseHeader(
        "struct Lawn { [[sfs::material]] std::string look; };", "Lawn.h");
    CHECK(result.type->fields.size() == 1u);
    CHECK(result.type->fields[0].material);
    CHECK(!result.type->fields[0].materials);
  }

  TEST(
      "[[sfs::emissionLight]] should mark a field as an emission light setting")
  {
    const ParseResult result =
        parseHeader("struct Lamp { [[sfs::emissionLight]] float lightRange = "
                    "1.0f; float other; };",
                    "Lamp.h");
    CHECK(result.type->fields.size() == 2u);
    CHECK(result.type->fields[0].emissionLight);
    CHECK(!result.type->fields[1].emissionLight);
    GeneratorOptions options;
    options.module = "game";
    options.function = "registerTypes";
    const std::string code = generateRegistration({*result.type}, options);
    CHECK(code.find("field.attributes.emissionLight = true;") !=
          std::string::npos);
  }

  TEST("[[sfs::showIf(field)]] should name the bool field a field shows with")
  {
    const ParseResult result =
        parseHeader("struct Lens { bool blur = false; [[sfs::showIf(blur)]] "
                    "float amount = 1.0f; [[sfs::showIf]] float broken; "
                    "[[sfs::showIf(!blur)]] float sharpness = 1.0f; };",
                    "Lens.h");
    CHECK(result.type->fields.size() == 4u);
    CHECK(result.type->fields[1].showIf == "blur");
    CHECK(result.type->fields[2].showIf.empty());
    CHECK(result.type->fields[3].showIf == "!blur"); // while it's off
    CHECK(!result.diagnostics.empty()); // [[sfs::showIf]] names none
    GeneratorOptions options;
    options.module = "game";
    options.function = "registerTypes";
    const std::string code = generateRegistration({*result.type}, options);
    CHECK(code.find("field.attributes.showIf = \"blur\";") !=
          std::string::npos);
  }

  TEST("a UI layer should be a game object script")
  {
    const ParseResult result = parseHeader(
        "class Hud : public sfs::ui::Layer { public: int coins = 0; };",
        "Hud.h");
    CHECK(result.type && result.type->kind == TypeKind::GameObject &&
          result.type->uiLayer);
    CHECK(result.diagnostics.empty());
    const ParseResult plain =
        parseHeader("class Player : public sfs::GameObject {};", "Player.h");
    CHECK(plain.type && !plain.type->uiLayer);
  }

  TEST("a UI header (*.ui.h) should be skipped; a plain header's struct with "
       "a body() is still a component")
  {
    const ParseResult view =
        parseHeader("struct Badge { std::string text; std::function<void()> "
                    "tap; sfs::ui::View body() const; };",
                    "Badge.ui.h");
    CHECK(!view.type && view.diagnostics.empty());
    const ParseResult plain = parseHeader(
        "struct Badge { float mass = 1.0f; int body() const; };", "Badge.h");
    CHECK(plain.type && plain.type->kind == TypeKind::Component &&
          plain.type->fields.size() == 1u);
  }

  TEST("a UI header's structs should be found, views by their body(), with "
       "their public data members")
  {
    const UiParseResult result = parseUiHeader(R"(
#pragma once
namespace farm {
struct JobRow { std::string id, what; std::string who; };
struct JobBoard
{
  std::vector<JobRow> jobs;
  static constexpr int kMost = 5;
  const int fixed = 1;
  JobRow* first = nullptr;
  sfs::ui::View body() const;
  struct Inner { int notMe; };
private:
  int secret = 0;
};
class Clock { public: std::string time; sfs::ui::View body() const { return {}; } };
template <typename T> struct Box { T value; sfs::ui::View body() const; };
}
sfs::ui::View look(sfs::ui::View view);
)",
                                               "ui/Farm.ui.h");
    CHECK(result.diagnostics.empty());
    CHECK(result.types.size() == 3u);
    if (result.types.size() == 3u)
    {
      const UiTypeModel& row = result.types[0];
      CHECK(row.qualifiedName == "farm::JobRow" && !row.view &&
            row.fields == std::vector<std::string>{"id", "what", "who"});
      const UiTypeModel& board = result.types[1];
      CHECK(board.name == "JobBoard" && board.view &&
            board.fields == std::vector<std::string>{"jobs"});
      const UiTypeModel& clock = result.types[2];
      CHECK(clock.view && clock.fields == std::vector<std::string>{"time"});
    }
  }

  TEST("UI previews should fill every type's fields and preview its views")
  {
    UiTypeModel row{
        "JobRow", "farm::JobRow", false, {"what"}, "/p/scripts/ui/Farm.ui.h"};
    UiTypeModel board{"JobBoard",
                      "farm::JobBoard",
                      true,
                      {"jobs", "canSow"},
                      "/p/scripts/ui/Farm.ui.h"};
    GeneratorOptions options;
    options.module = "GameUiPreview";
    options.function = "sfsRegisterModule";
    options.includeRoot = "/p/scripts";
    const std::string code = generateUiPreviews({row, board}, options);
    CHECK(code.find("#include \"ui/Farm.ui.h\"") != std::string::npos);
    CHECK(code.find("struct Placeholder<farm::JobRow>") != std::string::npos);
    CHECK(code.find("placeholder(value.canSow, \"Can Sow\", item);") !=
          std::string::npos);
    CHECK(code.find("placeholderView<farm::JobBoard>") != std::string::npos);
    CHECK(code.find("placeholderView<farm::JobRow>") == std::string::npos);
    CHECK(code.find("void sfsRegisterModule(sfs::reflect::TypeRegistry&) {}") !=
          std::string::npos);
  }

  TEST("a scriptable object may derive from another reflected type, the "
       "engine's or the game's")
  {
    const ParseResult tractor = parseHeader(R"(
      struct TractorSpec : public sfs::VehicleSpec
      {
        float hitchHeight = 0.5f;
      };
    )",
                                            "/project/scripts/TractorSpec.h");
    CHECK(tractor.type.has_value() && tractor.type->kind == TypeKind::Derived);
    CHECK(tractor.diagnostics.empty());
    CHECK(tractor.type->base == "VehicleSpec");
    CHECK(findField(*tractor.type, "hitchHeight") != nullptr);

    const ParseResult fancy = parseHeader(
        "namespace game { struct FancyItem : ItemDef { int sparkle = 1; }; }",
        "/project/scripts/FancyItem.h");
    CHECK(fancy.type && fancy.type->kind == TypeKind::Derived &&
          fancy.type->base == "ItemDef");

    GeneratorOptions options;
    options.module = "game";
    options.function = "sfsRegisterModule";
    options.includeRoot = "/project/scripts";
    const std::string code = generateRegistration({*tractor.type}, options);
    CHECK(code.find("derivedType<TractorSpec>(\"TractorSpec\", \"Tractor "
                    "Spec\", \"game\", \"VehicleSpec\")") != std::string::npos);
    CHECK(code.find("objectAs<TractorSpec>(object)->hitchHeight") !=
          std::string::npos);
    // Scriptable objects need nothing of the scripting runtime.
    CHECK(code.find("scriptRegistry.h") == std::string::npos);
  }

  TEST("a type should have one base, and not a template")
  {
    const ParseResult two = parseHeader(
        "struct Both : sfs::ScriptableObject, Other { int a = 0; };",
        "/project/scripts/Both.h");
    CHECK(hasDiagnostic(two, Severity::Error, "one base"));
    const ParseResult templated =
        parseHeader("struct Listy : std::vector<int> { int a = 0; };",
                    "/project/scripts/Listy.h");
    CHECK(hasDiagnostic(
        templated, Severity::Error, "isn't a base a script type can have"));
  }

  TEST("an AssetRef to a namespaced type should name it as registered")
  {
    const ParseResult car = parseHeader(
        "struct Car { sfs::AssetRef<sfs::VehicleSpec> spec; };", "Car.h");
    const FieldModel* spec = findField(*car.type, "spec");
    CHECK(spec && spec->kind == "AssetRef" &&
          spec->assetType == "sfs::VehicleSpec");
    GeneratorOptions options;
    options.module = "game";
    options.function = "registerTypes";
    const std::string code = generateRegistration({*car.type}, options);
    CHECK(code.find("sfs::AssetRef<sfs::VehicleSpec>>") != std::string::npos);
    CHECK(code.find("field.attributes.assetType = \"VehicleSpec\";") !=
          std::string::npos);
  }

  TEST("[[sfs::component]] types beside the file's type should be components")
  {
    const ParseResult result = parseHeader(R"(
      namespace game::parts {
      struct Tipper { float angle = 0.0f; };
      struct [[sfs::component]] TipBody {};
      struct NotMarked { int a = 0; };
      class [[sfs::component]] TipPivot { public: float lift = 1.0f; };
      }
    )",
                                           "Tipper.h");
    CHECK(result.diagnostics.empty());
    CHECK(result.type && result.type->name == "Tipper");
    CHECK(result.components.size() == 2);
    CHECK(result.components[0].name == "TipBody");
    CHECK(result.components[0].qualifiedName == "game::parts::TipBody");
    CHECK(result.components[0].kind == TypeKind::Component);
    CHECK(result.components[1].name == "TipPivot");
    CHECK(findField(result.components[1], "lift") != nullptr);
  }

  TEST("a header of only [[sfs::component]] types should not warn")
  {
    const ParseResult result = parseHeader(R"(
      namespace game { inline namespace v1 {
      struct [[sfs::component]] Body {};
      class [[sfs::component]] Pivot {};
      } }
    )",
                                           "TipParts.h");
    CHECK(result.diagnostics.empty());
    CHECK(!result.type.has_value());
    CHECK(result.components.size() == 2);
    CHECK(result.components[0].qualifiedName == "game::v1::Body");
    // A marked class is a component even with nothing in it.
    CHECK(result.components[1].name == "Pivot");
  }

  TEST("the file's type may be marked too, and stays the file's type")
  {
    const ParseResult result =
        parseHeader("struct [[sfs::component]] Body {};", "Body.h");
    CHECK(result.diagnostics.empty());
    CHECK(result.type && result.type->name == "Body");
    CHECK(result.components.empty());
  }

  TEST("nested and template [[sfs::component]] types should warn, not register")
  {
    const ParseResult result = parseHeader(R"(
      struct Outer { struct [[sfs::component]] Inner {}; int a = 0; };
      template <typename T> struct [[sfs::component]] Box { T value; };
      void make() { struct [[sfs::component]] Local {}; }
    )",
                                           "Outer.h");
    CHECK(result.type && result.type->name == "Outer");
    CHECK(result.components.empty());
    CHECK(
        hasDiagnostic(result, Severity::Warning, "component Inner is nested"));
    CHECK(hasDiagnostic(result, Severity::Warning, "component Box is nested"));
    CHECK(
        hasDiagnostic(result, Severity::Warning, "component Local is nested"));
  }

  TEST("a [[sfs::component]] type with a base should be an error")
  {
    const ParseResult result =
        parseHeader("struct Spec : sfs::ScriptableObject {};\n"
                    "struct [[sfs::component]] Bad : Other {};",
                    "Spec.h");
    CHECK(hasDiagnostic(
        result,
        Severity::Error,
        "A component can't derive from another type, but Bad derives "
        "from Other"));
    CHECK(result.components.empty());
  }

  TEST("an unknown [[sfs::...]] attribute on a type should warn")
  {
    const ParseResult result =
        parseHeader("struct [[sfs::compnent]] Body {};", "Body.h");
    CHECK(hasDiagnostic(
        result, Severity::Warning, "There is no attribute [[sfs::compnent]]."));
  }

  TEST("two types of one name should be an error, in a header or across them")
  {
    const ParseResult inOne = parseHeader(R"(
      namespace a { struct [[sfs::component]] Body {}; }
      namespace b { struct [[sfs::component]] Body {}; }
    )",
                                          "Parts.h");
    CHECK(inOne.components.size() == 1);
    CHECK(hasDiagnostic(
        inOne, Severity::Error, "More than one type is named Body"));

    const ParseResult first = parseHeader("struct Body {};", "/p/Body.h");
    const ParseResult second =
        parseHeader("struct [[sfs::component]] Body {};", "/p/Parts.h");
    const std::vector<Diagnostic> clash =
        checkNames({*first.type, second.components[0]});
    CHECK(clash.size() == 1);
    CHECK(severityOf(clash[0]) == Severity::Error);
    CHECK(clash[0].file == std::filesystem::path("/p/Parts.h"));
    CHECK(clash[0].message.find("/p/Body.h") != std::string::npos);
    CHECK(checkNames({*first.type}).empty());
  }

  TEST("a header's several types should be included once, the file's first")
  {
    const ParseResult result = parseHeader(R"(
      struct [[sfs::component]] TipBody {};
      struct Tipper { float angle = 0.0f; };
    )",
                                           "/project/scripts/Tipper.h");
    std::vector<TypeModel> types{*result.type};
    types.insert(
        types.end(), result.components.begin(), result.components.end());
    GeneratorOptions options;
    options.module = "game";
    options.function = "registerTypes";
    options.includeRoot = "/project/scripts";
    const std::string code = generateRegistration(types, options);
    const std::size_t include = code.find("#include \"Tipper.h\"");
    CHECK(include != std::string::npos);
    CHECK(code.find("#include \"Tipper.h\"", include + 1) == std::string::npos);
    const std::size_t tipper = code.find("componentType<Tipper>");
    const std::size_t body = code.find(
        "componentType<TipBody>(\"TipBody\", \"Tip Body\", \"game\")");
    CHECK(tipper != std::string::npos && body != std::string::npos &&
          tipper < body);
  }

  TEST("a single-type header's generated code should be unchanged")
  {
    const ParseResult result =
        parseHeader("struct Health { int hp = 3; };", "/s/Health.h");
    GeneratorOptions options;
    options.module = "game";
    options.function = "registerTypes";
    options.includeRoot = "/s";
    const std::string code = generateRegistration({*result.type}, options);
    const std::string expected =
        "// Generated by sfs-codegen -- do not edit.\n"
        "#include \"engine/core/reflection/reflection.h\"\n"
        "#include \"engine/core/reflection/registration.h\"\n\n"
        "\n"
        "#include \"Health.h\"\n"
        "\n#include <cstdint>\n";
    CHECK(code.starts_with(expected));
  }

  TEST("entity fields should be the Entity and EntityList kinds")
  {
    const ParseResult result = parseHeader(
        "namespace game { struct Rig { sfs::Entity arm; "
        "std::vector<sfs::Entity> blades; Entity bare; "
        "std::vector<Entity> bareList; std::array<sfs::Entity, 2> pair; "
        "[[sfs::hide]] sfs::Entity hidden; }; }",
        "Rig.h");
    CHECK(result.type->fields.size() == 2u);
    CHECK(findField(*result.type, "arm")->kind == "Entity");
    CHECK(findField(*result.type, "blades")->kind == "EntityList");
    // `Entity` alone is sfs::Entity only inside namespace sfs.
    CHECK(findField(*result.type, "bare") == nullptr);
    CHECK(findField(*result.type, "bareList") == nullptr);
    CHECK(findField(*result.type, "pair") == nullptr);
    CHECK(hasDiagnostic(result, Severity::Warning, "field pair is skipped"));

    const ParseResult engine =
        parseHeader("namespace sfs { struct Follow { Entity target; "
                    "std::vector<Entity> also; }; }",
                    "Follow.h");
    CHECK(engine.type->fields.size() == 2u);
    CHECK(engine.type->fields[0].kind == "Entity");
    CHECK(engine.type->fields[1].kind == "EntityList");
    CHECK(engine.diagnostics.empty());

    GeneratorOptions options;
    options.module = "game";
    options.function = "registerTypes";
    const std::string code = generateRegistration({*result.type}, options);
    CHECK(code.find("decltype(std::declval<game::Rig&>().arm)>, "
                    "sfs::Entity>") != std::string::npos);
    CHECK(code.find("decltype(std::declval<game::Rig&>().blades)>, "
                    "std::vector<sfs::Entity>>") != std::string::npos);
    CHECK(code.find("field.kind = FieldKind::Entity;") != std::string::npos);
    CHECK(code.find("field.kind = FieldKind::EntityList;") !=
          std::string::npos);
  }

  TEST("[[sfs::part]] should name an entity field's node; bad ones warn")
  {
    const ParseResult result = parseHeader(
        "struct Deck { [[sfs::part(\"deck\")]] sfs::Entity deck; "
        "[[sfs::part(\"blade-*\")]] std::vector<sfs::Entity> blades; "
        "[[sfs::part]] sfs::Entity none; "
        "[[sfs::part(\"a\", \"b\")]] sfs::Entity two; "
        "[[sfs::part(3)]] sfs::Entity number; "
        "[[sfs::part(\"say \\\"hi\\\"\")]] sfs::Entity quoted; "
        "[[sfs::part(\"x\")]] float notAnEntity = 0.0f; };",
        "Deck.h");
    CHECK(result.type->fields.size() == 7u);
    CHECK(findField(*result.type, "deck")->part == "deck");
    CHECK(findField(*result.type, "blades")->part == "blade-*");
    CHECK(findField(*result.type, "none")->part.empty());
    CHECK(findField(*result.type, "two")->part.empty());
    CHECK(findField(*result.type, "number")->part.empty());
    CHECK(findField(*result.type, "quoted")->part == "say \"hi\"");
    // On another kind of field it's ignored, with a warning.
    CHECK(findField(*result.type, "notAnEntity")->part.empty());
    const auto warnings = std::count_if(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [](const Diagnostic& d)
        {
          return severityOf(d) == Severity::Warning &&
                 d.message.find("[[sfs::part]]") != std::string::npos;
        });
    CHECK(warnings == 4);

    GeneratorOptions options;
    options.module = "game";
    options.function = "registerTypes";
    const std::string code = generateRegistration({*result.type}, options);
    CHECK(code.find("field.attributes.part = \"blade-*\";") !=
          std::string::npos);
    CHECK(code.find("field.attributes.part = \"say \\\"hi\\\"\";") !=
          std::string::npos);
  }

  TEST("an entity field on a scriptable object, system or game object should "
       "warn and be skipped")
  {
    for (const std::string_view base : {"sfs::ScriptableObject",
                                        "sfs::System",
                                        "sfs::GameObject",
                                        "sfs::VehicleSpec"})
    {
      const ParseResult result =
          parseHeader("struct Spec : public " + std::string(base) +
                          " { sfs::Entity target; "
                          "std::vector<sfs::Entity> list; float mass = 1; };",
                      "Spec.h");
      CHECK(result.type->fields.size() == 1u);
      CHECK(result.type->fields[0].name == "mass");
      CHECK(hasDiagnostic(result,
                          Severity::Warning,
                          "Entity reference fields belong on components"));
    }
  }

  TEST("the attribute table should be complete and consistent")
  {
    std::set<std::string_view> names;
    for (const AttributeInfo& info : attributes())
    {
      CHECK(names.insert(info.name).second);
      CHECK(!info.summary.empty());
      CHECK(findAttribute(info.name) == &info);
      for (const std::string_view kind : info.kinds)
        CHECK(kind.empty() || !cppTypeOfKind(kind).empty());
      // Snippet placeholders are numbered from 1, in order.
      int next = 1;
      for (std::size_t at = info.snippet.find("${"); at != std::string::npos;
           at = info.snippet.find("${", at + 2))
        CHECK(info.snippet.substr(at + 2, 1) == std::to_string(next++));
      CHECK((info.args == AttributeArgs::None) == info.arguments.empty());
      CHECK(info.snippet.empty() == info.arguments.empty());
    }
    CHECK(names.size() == attributes().size());
    CHECK(findAttribute("nope") == nullptr);
    CHECK(attributeFieldTypes(*findAttribute("color")) ==
          "glm::vec3 and glm::vec4");
    CHECK(attributeFieldTypes(*findAttribute("range"), "`") ==
          "`std::int32_t`, `std::uint32_t` and `float`");
    CHECK(attributeFieldTypes(*findAttribute("hide")).empty());
  }

  TEST("every attribute's example should use it, and read cleanly")
  {
    for (const AttributeInfo& info : attributes())
    {
      const std::string written = "[[sfs::" + std::string(info.name);
      CHECK(info.example.find(written) != std::string_view::npos);
      const bool type = info.target == AttributeTarget::Type;
      const std::string source =
          type ? std::string(info.example)
               : "struct Example\n{\n" + std::string(info.example) + "\n};\n";
      const ParseResult result = parseHeader(source, "Example.h");
      CHECK(result.diagnostics.empty());
      if (!result.diagnostics.empty())
        std::fprintf(stderr,
                     "%s: %s\n",
                     std::string(info.name).c_str(),
                     result.diagnostics[0].message.c_str());
      CHECK(type ? result.components.size() == 1u : result.type.has_value());
    }
  }

  TEST("every attribute should have a parser case")
  {
    const ParseResult result = parseHeader(R"(
      struct [[sfs::component]] All
      {
        bool on = false;
        [[sfs::range(0, 2)]] float r = 1;
        [[sfs::color]] glm::vec3 c;
        [[sfs::hide]] int hidden = 0;
        [[sfs::script]] std::string s;
        [[sfs::texture]] std::string t;
        [[sfs::materials]] std::vector<std::string> ms;
        [[sfs::material]] std::string m;
        [[sfs::emissionLight]] float e = 0;
        [[sfs::showIf(on)]] float shown = 0;
        [[sfs::part("p")]] sfs::Entity p;
      private:
        [[sfs::show]] int secret = 0;
      };
    )",
                                           "All.h");
    CHECK(result.diagnostics.empty());
    const TypeModel& type = *result.type;
    CHECK(findField(type, "r")->range.has_value());
    CHECK(findField(type, "c")->color);
    CHECK(findField(type, "hidden") == nullptr);
    CHECK(findField(type, "s")->script);
    CHECK(findField(type, "t")->texture);
    CHECK(findField(type, "ms")->materials);
    CHECK(findField(type, "m")->material);
    CHECK(findField(type, "e")->emissionLight);
    CHECK(findField(type, "shown")->showIf == "on");
    CHECK(findField(type, "p")->part == "p");
    CHECK(findField(type, "secret") != nullptr);
    CHECK(type.kind == TypeKind::Component);
  }

  TEST("the rule table should be complete, kebab-case and unique")
  {
    std::set<std::string_view> names;
    std::size_t index = 0;
    for (const RuleInfo& rule : rules())
    {
      CHECK(static_cast<std::size_t>(rule.id) == index++);
      CHECK(names.insert(rule.name).second);
      CHECK(!rule.summary.empty());
      CHECK(std::all_of(rule.name.begin(),
                        rule.name.end(),
                        [](char c)
                        { return (c >= 'a' && c <= 'z') || c == '-'; }));
      CHECK(&ruleInfo(rule.id) == &rule);
      CHECK(findRule(rule.name) == (rule.reserved ? nullptr : &rule));
    }
    CHECK(index == static_cast<std::size_t>(RuleId::Count));
    CHECK(!canIgnore(RuleId::DuplicateType));
    CHECK(!canIgnore(RuleId::BadIgnore));
    CHECK(canIgnore(RuleId::NoScriptType));
    CHECK(closestRule("own-entiy") == &ruleInfo(RuleId::OwnEntity));
    // Reserved rules aren't nameable, or listed, until something reports them.
    CHECK(ruleInfo(RuleId::RenamedEngineType).reserved);
    CHECK(findRule("renamed-engine-type") == nullptr);
    CHECK(closestRule("renamed-engine-typ") == nullptr);
    CHECK(ruleDocsTable().find("renamed-engine-type") == std::string::npos);
    CHECK(closestRule("something-else") == nullptr);
  }

  TEST("the entity API rules should follow the lint rows, tagged and ignorable")
  {
    // After stale-saved-name, in this order, before bad-ignore.
    const std::array<std::string_view, 8> names = {"has-then-get",
                                                   "is-alive",
                                                   "row-lookup",
                                                   "setup-in-constructor",
                                                   "component-pointer",
                                                   "filter-in-loop",
                                                   "unused-row-entity",
                                                   "row-component-copy"};
    std::size_t at = static_cast<std::size_t>(RuleId::StaleSavedName);
    for (const std::string_view name : names)
    {
      const RuleInfo& rule = rules()[++at];
      CHECK(rule.name == name);
      CHECK(rule.source == RuleSource::Lint);
      CHECK(rule.severity == Severity::Warning);
      CHECK(!rule.reserved);
      CHECK(canIgnore(rule.id));
      CHECK(ruleDocsTable().find("`" + std::string(name) + "`") !=
            std::string::npos);
    }
    CHECK(rules()[at + 1].id == RuleId::BadIgnore);
    CHECK(ruleInfo(RuleId::HasThenGet).unnecessary);
    CHECK(ruleInfo(RuleId::RowLookup).unnecessary);
    CHECK(ruleInfo(RuleId::UnusedRowEntity).unnecessary);
    CHECK(ruleInfo(RuleId::IsAlive).deprecated);
    CHECK(ruleInfo(RuleId::ComponentPointer).deprecated);
    CHECK(!ruleInfo(RuleId::SetupInConstructor).unnecessary &&
          !ruleInfo(RuleId::SetupInConstructor).deprecated);
  }

  TEST("every codegen diagnostic should carry its rule")
  {
    struct Case
    {
      std::string_view source;
      std::string_view file;
      RuleId rule;
    };
    const Case cases[] = {
        {"struct Other {};", "Mine.h", RuleId::NoScriptType},
        {"struct A {}; namespace b { struct A {}; }",
         "A.h",
         RuleId::DuplicateType},
        {"struct S : sfs::System, Other {};", "S.h", RuleId::ScriptBase},
        {"struct C {}; struct [[sfs::component]] D : Other {};",
         "C.h",
         RuleId::ComponentBase},
        {"struct S { struct [[sfs::component]] N {}; int a; };",
         "S.h",
         RuleId::ComponentNotRegistered},
        {"struct S { int* p; };", "S.h", RuleId::FieldType},
        {"struct S : sfs::System { sfs::Entity e; };",
         "S.h",
         RuleId::EntityField},
        {"struct S { [[sfs::hdie]] int a; };",
         "S.h",
         RuleId::UnknownSfsAttribute},
        {"struct S { [[sfs::component]] int a; };",
         "S.h",
         RuleId::AttributeTarget},
        {"struct S { [[sfs::range(1)]] int a; };",
         "S.h",
         RuleId::AttributeArguments},
        {"struct S { [[sfs::color]] float a; };",
         "S.h",
         RuleId::AttributeFieldType},
        {"struct S { [[sfs::showIf(nope)]] float a; };",
         "S.h",
         RuleId::ShowIfTarget},
        {"// sfs-ignore-file(nope)\nstruct S {};", "S.h", RuleId::BadIgnore},
    };
    for (const Case& c : cases)
    {
      const ParseResult result = parseHeader(c.source, c.file);
      CHECK(result.diagnostics.size() == 1u);
      CHECK(!result.diagnostics.empty() &&
            result.diagnostics[0].rule == c.rule);
    }
  }

  TEST("an unknown attribute should suggest the closest, with a fix")
  {
    const std::string source = "struct S {\n  [[sfs::hdie]] int a;\n  "
                               "[[sfs::Range(0, 1)]] float b;\n  "
                               "[[sfs::frobnicate]] int c; };";
    const ParseResult result = parseHeader(source, "S.h");
    const auto unknown = ofRule(result, RuleId::UnknownSfsAttribute);
    CHECK(unknown.size() == 3u);
    CHECK(unknown[0]->message ==
          "There is no attribute [[sfs::hdie]]. Use [[sfs::hide]] if that's "
          "what you meant.");
    CHECK(unknown[0]->range.startRow == 1u);
    CHECK(unknown[0]->fixTitle == "Change to [[sfs::hide]]");
    CHECK(applyFix(source, *unknown[0]).find("[[sfs::hide]] int a;") !=
          std::string::npos);
    CHECK(unknown[1]->message.find(
              "Use [[sfs::range]] if that's what you meant.") !=
          std::string::npos);
    CHECK(unknown[2]->message == "There is no attribute [[sfs::frobnicate]].");
    CHECK(unknown[2]->fix.empty());
    // The field without its attribute is still reflected.
    CHECK(findField(*result.type, "a") != nullptr);
  }

  TEST("an attribute on the wrong target should warn and be ignored")
  {
    const ParseResult onField =
        parseHeader("struct S { [[sfs::component]] int a; };", "S.h");
    CHECK(hasRule(
        onField,
        RuleId::AttributeTarget,
        "[[sfs::component]] goes on a type, not a field, so it's ignored "
        "here. Move it to the type."));
    CHECK(findField(*onField.type, "a") != nullptr);

    const ParseResult onType =
        parseHeader("struct [[sfs::hide]] Thing { int a; };", "Thing.h");
    CHECK(hasRule(
        onType,
        RuleId::AttributeTarget,
        "[[sfs::hide]] goes on a field, not a type, so it's ignored here. "
        "Move it to a field."));
    CHECK(onType.type.has_value());
  }

  TEST("each argument shape should be checked")
  {
    const ParseResult result = parseHeader(R"(
      struct S
      {
        bool on = false;
        [[sfs::range(0)]] float a = 0;
        [[sfs::range(0, 1)]] float b = 0;
        [[sfs::showIf(on, on)]] float c = 0;
        [[sfs::showIf(!on)]] float d = 0;
        [[sfs::showIf(1 + 2)]] float e = 0;
        [[sfs::hide()]] float f = 0;
        [[sfs::color(1)]] glm::vec3 g;
        [[sfs::part("")]] sfs::Entity h;
      };
    )",
                                           "S.h");
    CHECK(hasRule(result,
                  RuleId::AttributeArguments,
                  "[[sfs::range]] needs two arguments. Write (min, max)."));
    CHECK(!findField(*result.type, "a")->range.has_value());
    CHECK(findField(*result.type, "b")->range.has_value());
    CHECK(ofRule(result, RuleId::AttributeArguments).size() == 6u);
    CHECK(findField(*result.type, "c")->showIf.empty());
    CHECK(findField(*result.type, "d")->showIf == "!on");
    CHECK(findField(*result.type, "e")->showIf.empty());
    CHECK(hasRule(result,
                  RuleId::AttributeArguments,
                  "[[sfs::hide]] takes no arguments"));
    CHECK(findField(*result.type, "f") == nullptr); // still applied
    CHECK(hasRule(result,
                  RuleId::AttributeArguments,
                  "[[sfs::color]] takes no arguments"));
    CHECK(findField(*result.type, "g")->color);
    CHECK(hasRule(
        result,
        RuleId::AttributeArguments,
        "[[sfs::part]] needs one non-empty string. Write (\"node-name\")."));
  }

  TEST("an attribute on a field kind it doesn't apply to should be ignored")
  {
    const ParseResult result = parseHeader(
        "struct S { [[sfs::color]] float a = 0; [[sfs::part(\"x\")]] int b = "
        "0; [[sfs::range(0, 1)]] glm::vec3 c; [[sfs::texture]] float d = 0; "
        "[[sfs::color]] glm::vec4 ok; };",
        "S.h");
    CHECK(
        hasRule(result,
                RuleId::AttributeFieldType,
                "[[sfs::color]] is for glm::vec3 and glm::vec4 fields, so it's "
                "ignored here."));
    CHECK(
        hasRule(result,
                RuleId::AttributeFieldType,
                "[[sfs::part]] is for sfs::Entity and std::vector<sfs::Entity> "
                "fields, so it's ignored here."));
    CHECK(ofRule(result, RuleId::AttributeFieldType).size() == 4u);
    CHECK(!findField(*result.type, "a")->color);
    CHECK(findField(*result.type, "b")->part.empty());
    CHECK(!findField(*result.type, "c")->range.has_value());
    CHECK(!findField(*result.type, "d")->texture);
    CHECK(findField(*result.type, "ok")->color);
  }

  TEST("showIf should name a reflected bool field of the same type")
  {
    const ParseResult result = parseHeader(R"(
      struct S
      {
        bool on = false;
        float level = 0;
        [[sfs::hide]] bool hidden = false;
        [[sfs::showIf(on)]] float a = 0;
        [[sfs::showIf(!missing)]] float b = 0;
        [[sfs::showIf(level)]] float c = 0;
        [[sfs::showIf(hidden)]] float d = 0;
      };
    )",
                                           "S.h");
    const auto targets = ofRule(result, RuleId::ShowIfTarget);
    CHECK(targets.size() == 3u);
    CHECK(hasRule(result,
                  RuleId::ShowIfTarget,
                  "[[sfs::showIf(!missing)]] names missing, but S has no "
                  "reflected bool field by that name."));
    // The attribute is kept: the Inspector shows a field whose switch is gone.
    CHECK(findField(*result.type, "b")->showIf == "!missing");

    const ParseResult derived = parseHeader(
        "struct Spec : sfs::VehicleSpec { [[sfs::showIf(baseFlag)]] float a "
        "= 0; };",
        "Spec.h");
    CHECK(ofRule(derived, RuleId::ShowIfTarget).empty());
  }

  TEST("diagnostics should carry the node's range; no-script-type the first "
       "line")
  {
    const ParseResult result =
        parseHeader("#pragma once\nstruct S {\n  int*   p;\n};", "S.h");
    const auto field = ofRule(result, RuleId::FieldType);
    CHECK(field.size() == 1u);
    CHECK(field[0]->range.startRow == 2u);
    CHECK(field[0]->range.startColumn == 5u);
    CHECK(field[0]->range.endColumn == 10u);

    const ParseResult none = parseHeader("#pragma once\n\nint x;\n", "Mine.h");
    const auto missing = ofRule(none, RuleId::NoScriptType);
    CHECK(missing.size() == 1u);
    CHECK(missing[0]->range.startRow == 0u);
    CHECK(missing[0]->range.endRow == 0u);
    CHECK(missing[0]->range.endColumn == 12u);
  }

  TEST("sfs-ignore forms should hide their rules on the right lines only")
  {
    const std::string source =
        "#pragma once\n"
        "struct S\n"
        "{\n"
        "  int* a; // sfs-ignore(field-type)\n"
        "  int* b;\n"
        "  // sfs-ignore-next-line(field-type): a reason\n"
        "  int* c;\n"
        "  int* d; /* sfs-ignore( attribute-target ,  "
        "field-type ) */\n"
        "  int* e; // sfs-ignore(attribute-target)\n"
        "};\n";
    const ParseResult result = parseHeader(source, "S.h");
    const auto left = ofRule(result, RuleId::FieldType);
    CHECK(left.size() == 2u);
    CHECK(left.size() == 2u &&
          left[0]->message.find("field * b is") != std::string::npos);
    CHECK(left.size() == 2u &&
          left[1]->message.find("field * e is") != std::string::npos);
    CHECK(ofRule(result, RuleId::BadIgnore).empty());
    CHECK(result.suppressions.lines.size() == 4u);
    CHECK(result.suppressions.file.none());

    const ParseResult file = parseHeader(
        "#pragma once\n// sfs-ignore-file(no-script-type)\nint x;\n", "Mine.h");
    CHECK(file.diagnostics.empty());
    CHECK(file.suppressions.file.test(
        static_cast<std::size_t>(RuleId::NoScriptType)));
  }

  TEST("sfs-ignore in a string or later in a comment should not count")
  {
    const ParseResult result =
        parseHeader("const char* s = \"// sfs-ignore-file(no-script-type)\";\n"
                    "// see sfs-ignore-file(no-script-type)\n",
                    "Mine.h");
    CHECK(ofRule(result, RuleId::NoScriptType).size() == 1u);
    CHECK(result.suppressions.file.none());
  }

  TEST("errors should never be hidden")
  {
    const ParseResult result =
        parseHeader("// sfs-ignore-file(duplicate-type, script-base)\n"
                    "struct S : sfs::System, Other {};\n",
                    "S.h");
    CHECK(ofRule(result, RuleId::ScriptBase).size() == 1u);
    CHECK(hasRule(
        result,
        RuleId::BadIgnore,
        "The rule duplicate-type is an error, so it can't be ignored."));
    CHECK(hasRule(result,
                  RuleId::BadIgnore,
                  "The rule script-base is an error, so it can't be ignored."));
  }

  TEST("each bad sfs-ignore should warn, with fixes where one is clear")
  {
    const std::string source = "struct S {};\n"
                               "// sfs-ignore-file(own-entiy)\n"
                               "// sfs-ignore-file()\n"
                               "// sfs-ignore-next-line: no list\n"
                               "// sfs-ignore(field-type)\n"
                               "// sfs-ignore-file(field-type\n"
                               "// sfs-ignore-file(bad-ignore)\n"
                               "// sfs-ignored is another word\n";
    const ParseResult result = parseHeader(source, "S.h");
    const auto bad = ofRule(result, RuleId::BadIgnore);
    CHECK(bad.size() == 6u);
    if (bad.size() == 6u)
    {
      CHECK(bad[0]->message ==
            "There is no rule own-entiy to ignore. Use own-entity if that's "
            "what you meant.");
      CHECK(bad[0]->range.startRow == 1u);
      CHECK(bad[0]->range.startColumn == 19u);
      CHECK(applyFix(source, *bad[0]).find("sfs-ignore-file(own-entity)") !=
            std::string::npos);
      CHECK(
          bad[1]->message ==
          "This sfs-ignore comment names no rule. Name the rules it hides, as "
          "in sfs-ignore(own-entity).");
      CHECK(
          bad[2]->message ==
          "This sfs-ignore comment names no rule. Name the rules it hides, as "
          "in sfs-ignore(own-entity).");
      CHECK(bad[3]->message ==
            "This sfs-ignore comment covers only its own line, which has no "
            "code. Use sfs-ignore-next-line instead.");
      CHECK(applyFix(source, *bad[3])
                .find("// sfs-ignore-next-line(field-type)\n") !=
            std::string::npos);
      CHECK(bad[4]->message.find("isn't closed") != std::string::npos);
      CHECK(bad[5]->message ==
            "The rule bad-ignore can't be ignored. Fix the sfs-ignore comment "
            "instead.");
    }
    // A misspelt directive name hides nothing, and gets a did-you-mean.
    const std::string misspelt =
        "struct S {};\n// sfs-ignore-nextline(field-type)\n"
        "// sfs-ignore-all(field-type)\n";
    const ParseResult names = parseHeader(misspelt, "S.h");
    const auto namesBad = ofRule(names, RuleId::BadIgnore);
    CHECK(namesBad.size() == 2u);
    if (namesBad.size() == 2u)
    {
      CHECK(namesBad[0]->message ==
            "There is no directive sfs-ignore-nextline. Use "
            "sfs-ignore-next-line if that's what you meant.");
      CHECK(applyFix(misspelt, *namesBad[0])
                .find("// sfs-ignore-next-line(field-type)") !=
            std::string::npos);
      CHECK(namesBad[1]->message.starts_with(
          "There is no directive sfs-ignore-all."));
    }
    CHECK(names.suppressions.lines.empty());

    // bad-ignore can't hide itself.
    const ParseResult self = parseHeader(
        "struct S {};\n// sfs-ignore-file(bad-ignore, nope)\n", "S.h");
    CHECK(ofRule(self, RuleId::BadIgnore).size() == 2u);
  }

  TEST("sfs-codegen should print the rule, honour sfs-ignore and still fail "
       "on errors")
  {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "sfsCodegenCliTest";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "Quiet.h") << "#pragma once\n"
                                      "// sfs-ignore-file(no-script-type)\n"
                                      "int helper();\n";
    std::ofstream(dir / "Loud.h") << "int helper2();\n";
    const auto run = [&](const std::string& headers)
    {
      const std::string command = "\"" SFS_CODEGEN_PATH
                                  "\" --module m --function f --output \"" +
                                  (dir / "out.cpp").string() + "\" " + headers +
                                  " 2> \"" + (dir / "err.txt").string() + "\"";
      return std::system(command.c_str());
    };
    CHECK(run("\"" + (dir / "Quiet.h").string() + "\" \"" +
              (dir / "Loud.h").string() + "\"") == 0);
    const std::string printed = readText(dir / "err.txt");
    CHECK(printed.find("Quiet.h") == std::string::npos);
    CHECK(printed.find("Loud.h:1: warning: No class or struct in this header "
                       "is named after the file, Loud.") != std::string::npos);

    std::ofstream(dir / "Bad.h") << "// sfs-ignore-file(script-base)\n"
                                    "struct Bad : sfs::System, Other {};\n";
    CHECK(run("\"" + (dir / "Bad.h").string() + "\"") != 0);
    CHECK(readText(dir / "err.txt").find("error: ") != std::string::npos);
    std::filesystem::remove_all(dir);
  }

  TEST("the docs' attribute table should be the attribute table's")
  {
    const std::string block = docsBlock(
        std::filesystem::path(SFS_DOCS_DIR) / "core/reflection/index.md",
        "sfs-attributes");
    CHECK(block == attributeDocsTable());
    if (block != attributeDocsTable())
      std::fprintf(stderr,
                   "docs/core/reflection/index.md should hold:\n%s",
                   attributeDocsTable().c_str());
  }

  TEST("the docs' rule table should be the rule table's")
  {
    const std::string block = docsBlock(
        std::filesystem::path(SFS_DOCS_DIR) / "editor/sfs-lsp/index.md",
        "sfs-rules");
    CHECK(block == ruleDocsTable());
    if (block != ruleDocsTable())
      std::fprintf(stderr,
                   "docs/editor/sfs-lsp/index.md should hold:\n%s",
                   ruleDocsTable().c_str());
  }

  return testing::report("codegenTests");
}
