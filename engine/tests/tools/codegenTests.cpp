#include "../testHarness.h"

#include <sfs/codegen/generator.h>
#include <sfs/codegen/parser.h>

#include <algorithm>
#include <string>

using namespace sfs::codegen;

namespace
{
const FieldModel* findField(const TypeModel& type, std::string_view name)
{
  const auto it = std::find_if(type.fields.begin(),
                               type.fields.end(),
                               [&](const FieldModel& f) { return f.name == name; });
  return it != type.fields.end() ? &*it : nullptr;
}

bool hasDiagnostic(const ParseResult& result,
                   Diagnostic::Severity severity,
                   std::string_view text)
{
  return std::any_of(result.diagnostics.begin(),
                     result.diagnostics.end(),
                     [&](const Diagnostic& d)
                     {
                       return d.severity == severity &&
                              d.message.find(text) != std::string::npos;
                     });
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

  TEST("unsupported field types should warn and be skipped")
  {
    const ParseResult result = parseHeader(kPlayer, "Player.h");
    CHECK(findField(*result.type, "target") == nullptr);
    CHECK(findField(*result.type, "items") == nullptr);
    CHECK(hasDiagnostic(result, Diagnostic::Severity::Warning, "pointer"));
    CHECK(hasDiagnostic(result, Diagnostic::Severity::Warning, "std::vector<int>"));
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
    CHECK(hasDiagnostic(result, Diagnostic::Severity::Warning, "Enemy"));
  }

  TEST("an unsupported base should be an error")
  {
    const ParseResult result =
        parseHeader("class Boss : public Monster { public: int hp; };", "Boss.h");
    CHECK(hasDiagnostic(result, Diagnostic::Severity::Error, "Monster"));
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
    CHECK(displayName("MeshRendererComponent", true) == std::string("Mesh Renderer"));
    CHECK(displayName("PointLight3DComponent", true) == std::string("Point Light 3D"));
    CHECK(displayName("HTTPServer", false) == std::string("HTTP Server"));
    CHECK(displayName("Component", true) == std::string("Component"));
  }

  TEST("generated code should register fields and use access tags for private ones")
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
    CHECK(code.find("componentType<game::Crop>(\"Crop\", \"Crop\", \"game\")") !=
          std::string::npos);
    CHECK(code.find("field.attributes.hasRange = true;") != std::string::npos);
    CHECK(code.find("template struct MemberAccess<Access_game_Crop_secret, "
                    "&game::Crop::secret>;") != std::string::npos);
  }

  TEST("systems and game objects should register how to instantiate them")
  {
    TypeModel player = *parseHeader(kPlayer, "/project/scripts/Player.h").type;
    TypeModel growth = *parseHeader(
        "class Growth : public sfs::System {};", "/project/scripts/Growth.h").type;
    GeneratorOptions options;
    options.module = "game";
    options.function = "sfsRegisterModule";
    options.includeRoot = "/project/scripts";

    const std::string code = generateRegistration({player, growth}, options);
    CHECK(code.find("#include \"engine/runtime/scripting/scriptRegistry.h\"") !=
          std::string::npos);
    CHECK(code.find("std::make_unique<Player>()") != std::string::npos);
    CHECK(code.find("scene.addSystem<Growth>()") != std::string::npos);
    CHECK(code.find("sfs::scripts().add(std::move(script));") != std::string::npos);
    CHECK(code.find("static_assert(std::is_base_of_v<sfs::GameObject, Player>") !=
          std::string::npos);
  }

  TEST("mesh sources should be recognised and register their build function")
  {
    const ParseResult result = parseHeader(
        "class GameModels : public sfs::MeshSource { public: void build(sfs::MeshLibrary& l) override; };",
        "/project/scripts/GameModels.h");
    CHECK(result.type->kind == TypeKind::MeshSource);

    GeneratorOptions options;
    options.module = "game";
    options.function = "sfsRegisterModule";
    options.includeRoot = "/project/scripts";
    const std::string code = generateRegistration({*result.type}, options);
    CHECK(code.find("#include \"engine/runtime/rendering3d/meshSource.h\"") != std::string::npos);
    CHECK(code.find("script.kind = sfs::ScriptType::Kind::MeshSource;") != std::string::npos);
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

  return testing::report("codegenTests");
}
