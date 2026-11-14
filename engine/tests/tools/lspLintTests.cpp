#include "../testHarness.h"
#include "lspFeatures.h"
#include "lspTestSupport.h"
#include "server.h"

#include <sfs/codegen/parser.h>

#include <cstdio>
#include <sstream>

using namespace sfs::lsp;
using nlohmann::json;
using sfs::codegen::RuleId;

namespace
{

// A temporary SFS project with a few script headers, for the project index.
struct Project
{
  std::filesystem::path root =
      std::filesystem::temp_directory_path() / "sfsLspLintProject";
  ProjectIndex index;

  Project()
  {
    std::filesystem::remove_all(root);
    lspTest::writeFile(root / "project.sfsproj", "{}");
    lspTest::writeFile(root / "scripts/systems/FieldSystem.h",
                       "#pragma once\nclass FieldSystem : public sfs::System { "
                       "public: int map(); };\n");
    lspTest::writeFile(root / "scripts/systems/CropSystem.h",
                       "#pragma once\nclass CropSystem : public sfs::System { "
                       "public: void update(double) override; };\n");
    lspTest::writeFile(root / "scripts/components/Crop.h",
                       "#pragma once\nstruct Crop { float growth = 0; };\n");
    index.start(root);
    index.indexAll(lspTest::parser());
  }

  ~Project() { std::filesystem::remove_all(root); }
};

// The fix of the first diagnostic of `rule`, applied, and the result analysed
// again.
std::unique_ptr<Document> fixed(const Document& document,
                                RuleId rule,
                                const ProjectIndex* index = nullptr)
{
  const sfs::codegen::Diagnostic* diagnostic = lspTest::first(document, rule);
  if (!diagnostic)
    return lspTest::analysed(document.text, document.path, index);
  return lspTest::analysed(lspTest::applyEdits(document.text, diagnostic->fix),
                           document.path,
                           index);
}

// Fixtures that make every rule report, codegen's included: each a path
// under a project's scripts/ folder and its text.
std::vector<std::pair<std::string, std::string>> everyRuleFixtures()
{
  return {
      {"t/S.h",
       "struct S { [[sfs::hide]] int a; };\nnamespace b { struct S {}; }\n"},
      {"t/Spec.h", "struct [[sfs::component]] Spec : Other {};\n"},
      {"t/Boss.h", "struct Boss : A, B { [[sfs::hide]] int y; };\n"},
      {"t/Crop.h",
       "struct Crop\n{\n  [[sfs::hdie]] int a;\n  [[sfs::component]] int b;\n"
       "  [[sfs::range(1)]] float c;\n  [[sfs::color]] float d;\n"
       "  [[sfs::showIf(nope)]] int e;\n  int* f;\n  sfs::Entity g;\n};\n"
       "struct [[sfs::component]] Outer { struct [[sfs::component]] Inner "
       "{}; };\n"},
      {"t/Helper.h", "struct Other { [[sfs::hide]] int a; };\n"},
      {"t/Rig.h",
       "struct Rig : sfs::GameObject { "
       "[[sfs::hide]] int a; sfs::Entity target; "
       "};\n"},
      {"t/file.cpp",
       "using namespace sfs;\nclass Mover : public sfs::System\n{\n"
       "  std::map<sfs::Entity, int> m_map;\n"
       "  Mover()\n  {\n    registerComponent<A>();\n"
       "    registerComponent<Any<B, C>>();\n"
       "    registerComponent<Any<D, E>>();\n  }\n"
       "  void update(double) override\n  {\n    registerComponent<A>();\n"
       "    query<A, Any<B>>();\n    query<A, Any<A, B>>();\n"
       "    query<A, A>();\n    query<Any<A, B>, Excluding<A>>();\n"
       "    query<A, Excluding<A>>();\n"
       "    for (const sfs::Entity& e : registry->query<A>())\n"
       "      e.getComponent<A>().x = 1;\n"
       "    for (const sfs::Entity& e : registry->query<A>())\n"
       "      if (e.hasComponent<B>())\n"
       "        e.getComponent<B>()->y = 1;\n  }\n"
       "  void onEntityAdded(sfs::Entity e) override { "
       "registry->destroyEntity(e); }\n};\n"
       "void take(Tree& tree);\n"
       "void f(sfs::Entity entity)\n{\n  auto& s = entity.getComponent<S>();\n"
       "  s.entity = entity;\n  if (entity.isAlive()) {}\n"
       "  if (entity.hasComponent<B>())\n    entity.getComponent<B>().y = 1;\n"
       "  take(entity.getComponent<Tree>());\n}\n"
       "[[nodiscrad]] int g();\n// sfs-ignore(own-entiy)\n"},
      {"t/rows.cpp",
       "void g()\n{\n  for (auto [e, a] : registry->query<A>())\n"
       "    a.x = 1;\n  for (A a : registry->query<A>())\n    n += a.x;\n}\n"},
      {"systems/CropSystem.cpp",
       "void CropSystem::update(double) { registry->getSystem<FieldSystem>(); "
       "}\n"},
  };
}

const std::string kQueryHead = "using namespace sfs;\nvoid f()\n{\n";

} // namespace

int main()
{
  TEST("single-any should flag one-component groups, with a fix")
  {
    const auto doc = lspTest::analysed(
        "void f()\n{\n  scene().query<A, sfs::Any<B>>();\n}\n");
    CHECK(lspTest::count(*doc, RuleId::SingleAny) == 1u);
    const auto after = fixed(*doc, RuleId::SingleAny);
    CHECK(after->text.find("query<A, B>()") != std::string::npos);
    CHECK(after->diagnostics.empty());
    // A pack is not one component, and a game's own Any isn't SFS's.
    CHECK(lspTest::analysed("template <typename... Ts> void f() { query<A, "
                            "sfs::Any<Ts...>>(); }")
              ->diagnostics.empty());
    CHECK(lspTest::analysed("void f() { query<A, Any<B>>(); }")
              ->diagnostics.empty());
    CHECK(lspTest::count(
              *lspTest::analysed(kQueryHead + "  query<A, Any<B>>();\n}\n"),
              RuleId::SingleAny) == 1u);
    CHECK(
        lspTest::count(*lspTest::analysed(
                           "using sfs::Any;\nvoid f() { query<A, Any<B>>(); }"),
                       RuleId::SingleAny) == 1u);
    CHECK(lspTest::count(
              *lspTest::analysed(
                  "namespace sfs { void f() { query<A, Any<B>>(); } }"),
              RuleId::SingleAny) == 1u);
  }

  TEST("any-always-met, duplicate-term and excluded-any-member should flag and "
       "fix")
  {
    const auto always =
        lspTest::analysed(kQueryHead + "  query<A, Any<A, B>>();\n}\n");
    CHECK(lspTest::count(*always, RuleId::AnyAlwaysMet) == 1u);
    const auto alwaysFixed = fixed(*always, RuleId::AnyAlwaysMet);
    CHECK(alwaysFixed->text.find("query<A>()") != std::string::npos);
    CHECK(alwaysFixed->diagnostics.empty());

    const auto twice = lspTest::analysed(
        kQueryHead + "  query<A, B, A>();\n  query<Any<B, C, B>>();\n"
                     "  query<A, Excluding<C>, Excluding<C, D>>();\n}\n");
    CHECK(lspTest::count(*twice, RuleId::DuplicateTerm) == 3u);
    const auto twiceFixed = fixed(*twice, RuleId::DuplicateTerm);
    CHECK(twiceFixed->text.find("query<A, B>()") != std::string::npos);

    const auto excluded = lspTest::analysed(
        kQueryHead + "  query<Any<A, B>, Excluding<A>>();\n}\n");
    CHECK(lspTest::count(*excluded, RuleId::ExcludedAnyMember) == 1u);
    const auto excludedFixed = fixed(*excluded, RuleId::ExcludedAnyMember);
    CHECK(excludedFixed->text.find("query<Any<B>, Excluding<A>>()") !=
          std::string::npos);
    CHECK(lspTest::count(*excludedFixed, RuleId::SingleAny) ==
          1u); // then single-any follows

    CHECK(lspTest::analysed(kQueryHead +
                            "  query<A, Any<B, C>, Excluding<D>>();\n}\n")
              ->diagnostics.empty());
  }

  TEST("matches-nothing should flag terms that can never match, without a fix")
  {
    const auto doc = lspTest::analysed(
        kQueryHead + "  query<A, Excluding<A>>();\n  query<Any<B, C>, "
                     "Excluding<B, C>>();\n}\n");
    CHECK(lspTest::count(*doc, RuleId::MatchesNothing) == 2u);
    const auto* first = lspTest::first(*doc, RuleId::MatchesNothing);
    CHECK(first &&
          first->message ==
              "This query matches nothing, because A is both required and "
              "excluded. Keep it in only one of the two.");
    CHECK(first && first->fix.empty());
    CHECK(lspTest::analysed(kQueryHead + "  query<A, Excluding<B>>();\n}\n")
              ->diagnostics.empty());
  }

  TEST("a system's registerComponent set should be one term list")
  {
    const std::string source =
        "class Mover : public sfs::System\n{\npublic:\n  Mover()\n  {\n"
        "    registerComponent<A>();\n    registerComponent<sfs::Any<A, "
        "B>>();\n"
        "    registerComponent<sfs::Any<C, D>>();\n  }\n};\n";
    const auto doc = lspTest::analysed(source);
    CHECK(lspTest::count(*doc, RuleId::AnyAlwaysMet) == 1u);
    CHECK(lspTest::count(*doc, RuleId::SecondAny) == 1u);
    const auto after = fixed(*doc, RuleId::AnyAlwaysMet);
    CHECK(after->text.find("sfs::Any<A, B>") == std::string::npos);
    CHECK(after->text.find("    registerComponent<A>();\n    "
                           "registerComponent<sfs::Any<C, D>>();\n") !=
          std::string::npos);
    CHECK(lspTest::count(*after, RuleId::SecondAny) == 0u);

    // create() too, and out of line.
    const auto outOfLine = lspTest::analysed(
        "class Mover : public sfs::System { void create() override; };\n"
        "void Mover::create() { registerComponent<A>(); "
        "registerComponent<A>(); }\n");
    CHECK(lspTest::count(*outOfLine, RuleId::DuplicateTerm) == 1u);
    // Not a system: nothing.
    CHECK(lspTest::analysed("class Other { Other() { registerComponent<A>(); "
                            "registerComponent<A>(); } };")
              ->diagnostics.empty());
  }

  TEST("late-register and hook-mutates should flag calls in hooks and update")
  {
    const auto doc = lspTest::analysed(
        "class Mover : public sfs::System\n{\n  void update(double) override { "
        "registerComponent<A>(); }\n"
        "  void onEntityAdded(sfs::Entity e) override { e.addComponent<B>(); "
        "registry->destroyEntity(e); }\n"
        "  void helper() { registerComponent<A>(); addComponent<B>(); }\n"
        "  void create() override { observe<A>([this](sfs::Entity e) { "
        "e.removeComponent<A>(); registerComponent<C>(); }); }\n"
        "};\n");
    CHECK(lspTest::count(*doc, RuleId::LateRegister) == 2u);
    // A hook may change its own entity's components; destroying one is flagged.
    CHECK(lspTest::count(*doc, RuleId::HookMutates) == 1u);
  }

  TEST("hook-mutates should allow a hook's own entity and flag everything else")
  {
    // Written like the engine's TransformSystem.
    const auto own = lspTest::analysed(
        "class CacheSystem : public sfs::System { void onEntityAdded(const "
        "sfs::Entity& entity) override; void onEntityRemoved(sfs::Entity "
        "entity) override; };\n"
        "void CacheSystem::onEntityAdded(const sfs::Entity& entity) { "
        "sfs::Entity(entity).addComponent<Cache>(); "
        "registry->addTag<Seen>(entity); }\n"
        "void CacheSystem::onEntityRemoved(sfs::Entity entity) { "
        "entity.removeComponent<Cache>(); }\n");
    CHECK(lspTest::count(*own, RuleId::HookMutates) == 0u);
    const auto others = lspTest::analysed(
        "class CacheSystem : public sfs::System\n{\n"
        "  void onEntityAdded(sfs::Entity entity) override\n  {\n"
        "    target.addComponent<Cache>();\n"
        "    registry->removeComponent<Cache>(other);\n"
        "    registry->createEntity();\n"
        "    registry->addSystem<More>();\n  }\n"
        "  void create() override { observe<A>(nullptr, [this](sfs::Entity e) "
        "{ e.removeComponent<B>(); registry->destroyEntity(e); }); }\n};\n");
    CHECK(lspTest::count(*others, RuleId::HookMutates) == 5u);
    const auto* first = lspTest::first(*others, RuleId::HookMutates);
    CHECK(first && first->message.starts_with(
                       "This onEntityAdded() runs while the lists are being "
                       "updated, so it should change only its own entity's "
                       "components."));
  }

  TEST("the walk should survive code nested tens of thousands deep")
  {
    std::string sum = "int f() { return 1";
    for (int i = 0; i < 60000; ++i)
      sum += "+1";
    sum += "; }\n";
    const auto deepSum = lspTest::analysed(sum, "/t/deep.cpp");
    CHECK(deepSum->diagnostics.empty());
    std::string nested = "struct Deep { int a = ";
    nested +=
        std::string(30000, '(') + "1" + std::string(30000, ')') + "; };\n";
    const auto deepHeader = lspTest::analysed(nested, "/t/Deep.h");
    CHECK(deepHeader->tree != nullptr);
    const sfs::codegen::ParseResult codegen = sfs::codegen::parseHeader(
        "struct [[sfs::component]] Deep { int a = " + std::string(30000, '(') +
            "1" + std::string(30000, ')') + "; };\n",
        "Deep.h");
    CHECK(codegen.type.has_value());
  }

  TEST("system-reads-system should flag the project's systems, in systems only")
  {
    Project project;
    CHECK(project.index.isSystem("FieldSystem"));
    CHECK(!project.index.isSystem("Crop"));
    const std::string source =
        "class CropSystem : public sfs::System { void update(double) override; "
        "};\n"
        "void CropSystem::update(double)\n{\n  auto& fields = "
        "registry->getSystem<FieldSystem>();\n"
        "  auto* render = registry->tryGetSystem<sfs::RenderSystem>();\n  "
        "registry->hasSystem<CropSystem>();\n}\n"
        "void freeFunction(sfs::Registry* registry) { "
        "registry->getSystem<FieldSystem>(); }\n";
    const auto doc =
        lspTest::analysed(source,
                          project.root / "scripts/systems/CropSystem.cpp",
                          &project.index);
    CHECK(lspTest::count(*doc, RuleId::SystemReadsSystem) == 1u);
    const auto* found = lspTest::first(*doc, RuleId::SystemReadsSystem);
    CHECK(found && found->range.startRow == 3u);
    // An out-of-line method of a system the index knows, with no class in the
    // file.
    const auto split =
        lspTest::analysed("void CropSystem::lateUpdate() { "
                          "registry->getSystem<game::FieldSystem>(); }\n",
                          project.root / "scripts/systems/CropUpdate.cpp",
                          &project.index);
    CHECK(lspTest::count(*split, RuleId::SystemReadsSystem) == 1u);
    // Without an index (no project), the lint is off.
    CHECK(lspTest::count(
              *lspTest::analysed(source), RuleId::SystemReadsSystem) == 0u);
  }

  TEST("per-entity-map should flag maps keyed by entity in systems")
  {
    const auto doc =
        lspTest::analysed("class Mover : public sfs::System\n{\n  "
                          "std::unordered_map<sfs::Entity, int> m_a;\n"
                          "  std::set<sfs::Entity::EntityId> m_b;\n  static "
                          "std::map<sfs::Entity, int> s_c;\n"
                          "  std::map<int, sfs::Entity> m_d;\n  "
                          "std::vector<sfs::Entity> m_e;\n};\n"
                          "struct Plain { std::map<sfs::Entity, int> m; };\n");
    CHECK(lspTest::count(*doc, RuleId::PerEntityMap) == 2u);
  }

  TEST("own-entity should flag a component given its own entity")
  {
    const auto doc = lspTest::analysed(
        "void f(sfs::Entity entity)\n{\n  Vehicle& vehicle = "
        "entity.getComponent<Vehicle>();\n"
        "  State& state = vehicle;\n  state.entity = entity;\n"
        "  auto* p = &registry->getComponent<Part>(entity);\n  p->owner = "
        "entity;\n"
        "  entity.getComponent<Tag>().self = entity;\n  state.other = "
        "target;\n}\n");
    CHECK(lspTest::count(*doc, RuleId::OwnEntity) == 3u);
    const auto* first = lspTest::first(*doc, RuleId::OwnEntity);
    CHECK(first &&
          first->message.starts_with(
              "A component should not store the entity it's on, and Vehicle "
              "is given it here (state.entity = entity)."));
    // Reassigned in the body: it may be another entity by then.
    CHECK(lspTest::count(
              *lspTest::analysed("void f(sfs::Entity e) { auto& c = "
                                 "e.getComponent<C>(); e = other(); c.link = "
                                 "e; }"),
              RuleId::OwnEntity) == 0u);
    // Declared again by a loop, a lambda or a local: another entity there.
    const auto shadowed = lspTest::analysed(
        "void f(sfs::Entity entity)\n{\n"
        "  auto& cache = entity.getComponent<Holder>();\n"
        "  for (sfs::Entity entity : registry->query<Target>()) cache.target = "
        "entity;\n"
        "  auto set = [&](sfs::Entity entity) { cache.target = entity; };\n}\n"
        "void g(sfs::Entity entity)\n{\n"
        "  auto& cache = entity.getComponent<Holder>();\n"
        "  { sfs::Entity entity; cache.target = entity; }\n}\n");
    CHECK(lspTest::count(*shadowed, RuleId::OwnEntity) == 0u);
    // Declared before the binding (the parameter itself): still counted.
    CHECK(
        lspTest::count(*lspTest::analysed("void f(sfs::Entity e) { auto& c = "
                                          "e.getComponent<C>(); c.self = e; }"),
                       RuleId::OwnEntity) == 1u);
  }

  TEST("query term lists of other libraries should be left alone")
  {
    CHECK(lspTest::analysed("void f() { db.query<int, int>(\"SELECT a, b\"); }")
              ->diagnostics.empty());
    CHECK(lspTest::count(
              *lspTest::analysed(
                  "void f() { registry->query<A, A>(); "
                  "scene().query<B, B>(); m_registry.query<C, C>(); }"),
              RuleId::DuplicateTerm) == 3u);
    // An SFS wrapper makes any receiver's list SFS's.
    CHECK(lspTest::count(*lspTest::analysed("void f() { reg.query<A, "
                                            "sfs::Excluding<A>>(); }"),
                         RuleId::MatchesNothing) == 1u);
  }

  TEST("unknown-attribute should flag non-standard and misspelt attributes")
  {
    const auto doc = lspTest::analysed(
        "[[nodiscrad]] int f();\n[[nodiscard, maybe_unused]] int g();\n"
        "struct S { [[hide]] int a; [[sf::hide]] int b; [[gnu::hott]] int c; "
        "[[weird]] int d; };\n",
        "/t/notCodegen.cpp");
    CHECK(lspTest::count(*doc, RuleId::UnknownAttribute) == 4u);
    // The reserved spellings GCC and Clang accept are standard too.
    CHECK(lspTest::analysed("[[__nodiscard__, __maybe_unused__]] int h();\n",
                            "/t/notCodegen.cpp")
              ->diagnostics.empty());
    const auto* first = lspTest::first(*doc, RuleId::UnknownAttribute);
    CHECK(first &&
          first->message ==
              "There is no attribute nodiscrad. Use nodiscard if that's what "
              "you meant.");
    const auto after = fixed(*doc, RuleId::UnknownAttribute);
    CHECK(after->text.starts_with("[[nodiscard]] int f();"));
    std::vector<sfs::codegen::TextEdit> all;
    for (const auto& d : doc->diagnostics)
      all.insert(all.end(), d.fix.begin(), d.fix.end());
    const std::string text = lspTest::applyEdits(doc->text, all);
    CHECK(text.find("[[sfs::hide]] int a; [[sfs::hide]] int b;") !=
          std::string::npos);
  }

  TEST(
      "each lint should be hidden by each sfs-ignore form and by disabledRules")
  {
    const std::string line =
        "  query<A, sfs::Any<B>>(); // sfs-ignore(single-any)\n";
    const std::string next = "  // sfs-ignore-next-line(single-any): a "
                             "reason\n  query<A, sfs::Any<B>>();\n";
    const std::string file = "// sfs-ignore-file(single-any)\n";
    const std::string bare = "  query<A, sfs::Any<B>>();\n";
    CHECK(
        lspTest::analysed("void f()\n{\n" + line + "}\n")->diagnostics.empty());
    CHECK(
        lspTest::analysed("void f()\n{\n" + next + "}\n")->diagnostics.empty());
    CHECK(lspTest::analysed(file + "void f()\n{\n" + bare + "}\n")
              ->diagnostics.empty());
    sfs::codegen::RuleSet disabled;
    CHECK(disableRule("single-any", disabled).empty());
    CHECK(!disableRule("duplicate-type", disabled).empty());
    CHECK(!disableRule("bad-ignore", disabled).empty());
    CHECK(lspTest::analysed(
              "void f()\n{\n" + bare + "}\n", "/t/file.cpp", nullptr, disabled)
              ->diagnostics.empty());
    // A comment hides only the rules it names, on its own line.
    const auto other = lspTest::analysed(
        "void f()\n{\n  query<A, sfs::Any<B>>(); // sfs-ignore(own-entity)\n" +
        bare + "}\n");
    CHECK(lspTest::count(*other, RuleId::SingleAny) == 2u);
    const auto typo =
        lspTest::analysed("void f()\n{\n  query<A, sfs::Any<B>>(); // "
                          "sfs-ignore(single-anyy)\n}\n");
    CHECK(lspTest::count(*typo, RuleId::BadIgnore) == 1u);
  }

  TEST("the ignore actions should insert or extend comments, and leave the "
       "warning gone")
  {
    const ClientSettings settings;
    const auto doc = lspTest::analysed(
        "#pragma once\nvoid f()\n{\n    query<A, sfs::Any<B>>();\n}\n",
        "/t/f.h");
    const auto* diagnostic = lspTest::first(*doc, RuleId::SingleAny);
    CHECK(diagnostic != nullptr);
    if (!diagnostic)
      continue;
    const std::string onLine = lspTest::applyEdits(
        doc->text, ignoreEdits(*doc, RuleId::SingleAny, 3, false));
    CHECK(
        onLine.find("{\n    // sfs-ignore-next-line(single-any)\n    query") !=
        std::string::npos);
    CHECK(lspTest::analysed(onLine, "/t/f.h")->diagnostics.empty());
    const std::string inFile = lspTest::applyEdits(
        doc->text, ignoreEdits(*doc, RuleId::SingleAny, 0, true));
    CHECK(inFile.starts_with(
        "#pragma once\n// sfs-ignore-file(single-any)\nvoid f()"));
    CHECK(lspTest::analysed(inFile, "/t/f.h")->diagnostics.empty());

    // Appended to a comment above or on the line, or to the file's comment.
    const auto above = lspTest::analysed(
        "void f()\n{\n  // sfs-ignore-next-line(own-entity)\n  query<A, "
        "sfs::Any<B>>();\n}\n");
    const std::string extended = lspTest::applyEdits(
        above->text, ignoreEdits(*above, RuleId::SingleAny, 3, false));
    CHECK(extended.find("// sfs-ignore-next-line(own-entity, single-any)\n") !=
          std::string::npos);
    const auto trailing =
        lspTest::analysed("void f()\n{\n  query<A, sfs::Any<B>>(); // "
                          "sfs-ignore(own-entity)\n}\n");
    CHECK(
        lspTest::applyEdits(
            trailing->text, ignoreEdits(*trailing, RuleId::SingleAny, 2, false))
            .find("// sfs-ignore(own-entity, single-any)") !=
        std::string::npos);
    const auto top = lspTest::analysed("// sfs-ignore-file(own-entity)\nvoid "
                                       "f() { query<A, sfs::Any<B>>(); }\n");
    const std::string topExtended = lspTest::applyEdits(
        top->text, ignoreEdits(*top, RuleId::SingleAny, 0, true));
    CHECK(topExtended.starts_with(
        "// sfs-ignore-file(own-entity, single-any)\n"));
    CHECK(lspTest::analysed(topExtended)->diagnostics.empty());
    const auto noPragma =
        lspTest::analysed("void f() { query<A, sfs::Any<B>>(); }\n");
    CHECK(
        lspTest::applyEdits(
            noPragma->text, ignoreEdits(*noPragma, RuleId::SingleAny, 0, true))
            .starts_with("// sfs-ignore-file(single-any)\nvoid f()"));

    // The code actions: the fix first, then the two ignores; none for errors.
    const json actions = codeActions(*doc, {3, 0}, {3, 0}, settings);
    CHECK(actions.size() == 3u);
    CHECK(actions.size() == 3u && actions[0]["isPreferred"] == true);
    CHECK(actions.size() == 3u &&
          actions[1]["title"] == "Ignore single-any on this line");
    const auto error = lspTest::analysed(
        "struct S { [[sfs::hide]] int a; };\nnamespace b { struct S {}; }\n",
        "/t/S.h");
    CHECK(error->diagnostics.size() == 1u &&
          lspTest::count(*error, RuleId::DuplicateType) == 1u);
    CHECK(codeActions(*error, {1, 0}, {1, 0}, settings).empty());
  }

  TEST("the project index should know systems in and out of class and follow "
       "saves")
  {
    Project project;
    CHECK(project.index.isScriptHeader(project.root /
                                       "scripts/systems/FieldSystem.h"));
    CHECK(!project.index.isScriptHeader(project.root / "other/X.h"));
    lspTest::writeFile(project.root / "scripts/systems/NewSystem.h",
                       "#pragma once\nstruct NewSystem : sfs::System {};\n");
    project.index.rescan();
    CHECK(project.index.pending());
    project.index.indexAll(lspTest::parser());
    CHECK(project.index.isSystem("NewSystem"));
    std::filesystem::remove(project.root / "scripts/systems/NewSystem.h");
    project.index.rescan();
    CHECK(!project.index.isSystem("NewSystem"));
    const auto edited =
        lspTest::document("#pragma once\nstruct FieldSystem {};\n",
                          project.root / "scripts/systems/FieldSystem.h");
    project.index.update(edited->path,
                         sfs::codegen::parseHeader(
                             edited->text, edited->path, edited->tree.get()),
                         edited->tree.get(),
                         edited->text);
    CHECK(!project.index.isSystem("FieldSystem"));
  }

  TEST("--check over a fixture tree should print the golden listing")
  {
    Project project;
    lspTest::writeFile(project.root / "scripts/systems/CropSystem.cpp",
                       "void CropSystem::update(double)\n{\n  "
                       "registry->getSystem<FieldSystem>();\n}\n");
    lspTest::writeFile(project.root / "scripts/utils/Helpers.h",
                       "#pragma once\nint helper();\n");
    const std::filesystem::path output = project.root / "out.txt";
    const std::string command = "\"" SFS_LSP_PATH "\" --check \"" +
                                (project.root / "scripts").string() +
                                "\" > \"" + output.string() + "\"";
    CHECK(std::system(command.c_str()) == 0);
    std::ifstream in(output);
    std::stringstream printed;
    printed << in.rdbuf();
    const std::string scripts = (project.root / "scripts").string();
    const std::string golden =
        scripts +
        "/systems/CropSystem.cpp:3:3: warning: Systems should not read other "
        "systems. Put what CropSystem needs from FieldSystem on a component, "
        "which FieldSystem writes and CropSystem reads, or have CropSystem "
        "write a request field there that FieldSystem fills in. "
        "[system-reads-system]\n" +
        scripts +
        "/utils/Helpers.h:1:1: warning: No class or struct in this header is "
        "named after the file, Helpers. Name the script type Helpers, or mark "
        "a helper header with sfs-ignore-file(no-script-type). "
        "[no-script-type]\n";
    CHECK(printed.str() == golden);
  }

  TEST("has-then-get should flag a has-test guarding a lookup, and fix each "
       "shape")
  {
    // (b) if (E.has<T>()) S
    const auto plain = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n  if (e.hasComponent<Hitch>())\n"
        "    e.getComponent<Hitch>().x = 1;\n}\n");
    CHECK(lspTest::count(*plain, RuleId::HasThenGet) == 1u);
    const auto* found = lspTest::first(*plain, RuleId::HasThenGet);
    CHECK(found && found->range.startRow == 2u &&
          found->message ==
              "This looks Hitch up twice, with hasComponent<Hitch>() and then "
              "getComponent<Hitch>(). Use getComponent<Hitch>() alone, which "
              "gives null when Hitch is missing.");
    const auto plainFixed = fixed(*plain, RuleId::HasThenGet);
    CHECK(plainFixed->text ==
          "void f(sfs::Entity e)\n{\n  if (Hitch* hitch = "
          "e.getComponent<Hitch>())\n    hitch->x = 1;\n}\n");
    CHECK(plainFixed->diagnostics.empty());

    // (c) the test first, or after E.isValid(); a bound reference lends its
    // name.
    const auto chained = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n"
        "  if (e.isValid() && e.hasComponent<Hitch>() && ready)\n  {\n"
        "    const Hitch& h = e.getComponent<Hitch>();\n    use(h.y, &h);\n"
        "  }\n}\n");
    const auto chainedFixed = fixed(*chained, RuleId::HasThenGet);
    CHECK(chainedFixed->text ==
          "void f(sfs::Entity e)\n{\n"
          "  if (const Hitch* h = e.getComponent<Hitch>(); h && ready)\n  {\n"
          "    use(h->y, h);\n  }\n}\n");
    CHECK(chainedFixed->diagnostics.empty());

    // (d) the negated early-out, with the handle's validity test dropped.
    const auto early = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n"
        "  if (!e.isValid() || !e.hasComponent<sfs::TransformComponent>())\n"
        "    return;\n  e.getComponent<sfs::TransformComponent>().position = "
        "{};\n  move(e.getComponent<sfs::TransformComponent>());\n}\n");
    const auto earlyFixed = fixed(*early, RuleId::HasThenGet);
    CHECK(earlyFixed->text ==
          "void f(sfs::Entity e)\n{\n"
          "  sfs::TransformComponent* transform = "
          "e.getComponent<sfs::TransformComponent>();\n  if (!transform)\n"
          "    return;\n  transform->position = {};\n  move(*transform);\n}\n");
    CHECK(earlyFixed->diagnostics.empty());

    // (a) the pointer ternary, the old form and the new.
    const auto ternary = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n  Motion* m = e.hasComponent<Motion>() ? "
        "&e.getComponent<Motion>() : nullptr;\n  Motion* n = "
        "registry->hasComponent<Motion>(e) ? "
        "registry->getComponent<Motion>(e) : nullptr;\n}\n");
    CHECK(lspTest::count(*ternary, RuleId::HasThenGet) == 2u);
    std::vector<sfs::codegen::TextEdit> both;
    for (const auto& d : ternary->diagnostics)
      if (d.rule == RuleId::HasThenGet)
        both.insert(both.end(), d.fix.begin(), d.fix.end());
    CHECK(lspTest::applyEdits(ternary->text, both) ==
          "void f(sfs::Entity e)\n{\n  Motion* m = e.getComponent<Motion>();\n"
          "  Motion* n = registry->getComponent<Motion>(e);\n}\n");

    // After the handle's validity test, and inside parentheses.
    const auto valid = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n  Motion* m = e.isValid() && "
        "e.hasComponent<Motion>() ? &e.getComponent<Motion>() : nullptr;\n"
        "  if ((e.isValid() && e.hasComponent<Fault>()))\n"
        "    e.getComponent<Fault>().on = true;\n}\n");
    std::vector<sfs::codegen::TextEdit> each;
    for (const auto& d : valid->diagnostics)
      if (d.rule == RuleId::HasThenGet)
        each.insert(each.end(), d.fix.begin(), d.fix.end());
    CHECK(lspTest::applyEdits(valid->text, each) ==
          "void f(sfs::Entity e)\n{\n  Motion* m = e.getComponent<Motion>();\n"
          "  if (Fault* fault = e.getComponent<Fault>())\n"
          "    fault->on = true;\n}\n");

    // A value ternary is reported without a fix; asking alone isn't flagged.
    const auto value = lspTest::analysed(
        "float f(sfs::Entity e)\n{\n  bool has = e.hasComponent<A>();\n"
        "  return e.hasComponent<A>() ? e.getComponent<A>().v : 0.0f;\n}\n");
    CHECK(lspTest::count(*value, RuleId::HasThenGet) == 1u);
    CHECK(lspTest::first(*value, RuleId::HasThenGet) &&
          lspTest::first(*value, RuleId::HasThenGet)->fix.empty());
  }

  TEST("has-then-get should skip other handles, changed handles and removal "
       "hooks, and refuse clashing names")
  {
    CHECK(lspTest::count(
              *lspTest::analysed(
                  "void f(sfs::Entity a, sfs::Entity b)\n{\n  if "
                  "(a.hasComponent<T>())\n    b.getComponent<T>().x = 1;\n"
                  "  if (a.hasComponent<T>())\n  {\n    a = other();\n"
                  "    a.getComponent<T>().x = 1;\n  }\n}\n"),
              RuleId::HasThenGet) == 0u);
    // A component being removed is still there to look up, but has says no.
    const std::string hooks =
        "class S : public sfs::System\n{\n"
        "  void onEntityRemoved(const sfs::Entity& e) override\n  {\n"
        "    if (e.hasComponent<T>())\n      e.getComponent<T>().x = 1;\n  }\n"
        "  void create() override\n  {\n    observe<T>(nullptr, [](const "
        "sfs::Entity& e) { if (e.hasComponent<T>()) "
        "e.getComponent<T>().x = 1; });\n  }\n};\n";
    CHECK(lspTest::count(*lspTest::analysed(hooks), RuleId::HasThenGet) == 0u);
    // The name T gives is the handle's own: reported, not fixed.
    const auto clash = lspTest::analysed(
        "void f(sfs::Entity vehicle)\n{\n  if "
        "(vehicle.hasComponent<sfs::VehicleComponent>())\n"
        "    vehicle.getComponent<sfs::VehicleComponent>().on = true;\n}\n");
    CHECK(lspTest::count(*clash, RuleId::HasThenGet) == 1u);
    CHECK(lspTest::first(*clash, RuleId::HasThenGet) &&
          lspTest::first(*clash, RuleId::HasThenGet)->fix.empty());
    // Hidden like any lint.
    CHECK(lspTest::count(
              *lspTest::analysed(
                  "void f(sfs::Entity e)\n{\n  // "
                  "sfs-ignore-next-line(has-then-get)\n  if "
                  "(e.hasComponent<A>())\n    e.getComponent<A>().x = 1;\n}\n"),
              RuleId::HasThenGet) == 0u);
  }

  TEST("is-alive should flag handles it can type, and fold pairs")
  {
    const auto doc = lspTest::analysed(
        "struct Holder { sfs::Entity target; };\n"
        "void f(const sfs::Entity& e, Creature c, Holder& h)\n{\n"
        "  if (e.isValid() && e.isAlive()) {}\n"
        "  if (!h.target.isValid() || !h.target.isAlive()) {}\n"
        "  if (h.target.isAlive()) {}\n  if (c.isAlive()) {}\n"
        "  registry->isAlive(e);\n  for (const sfs::Entity& x : list) if "
        "(!x.isAlive()) {}\n}\n");
    CHECK(lspTest::count(*doc, RuleId::IsAlive) == 5u);
    std::vector<sfs::codegen::TextEdit> all;
    for (const auto& d : doc->diagnostics)
      all.insert(all.end(), d.fix.begin(), d.fix.end());
    const std::string text = lspTest::applyEdits(doc->text, all);
    CHECK(text.find("if (e.isValid()) {}") != std::string::npos);
    CHECK(text.find("if (!h.target.isValid()) {}") != std::string::npos);
    CHECK(text.find("if (h.target.isValid()) {}") != std::string::npos);
    CHECK(text.find("if (c.isAlive()) {}") != std::string::npos);
    CHECK(text.find("registry->isValid(e);") != std::string::npos);
    CHECK(text.find("if (!x.isValid()) {}") != std::string::npos);
    CHECK(lspTest::count(*lspTest::analysed(text), RuleId::IsAlive) == 0u);
  }

  TEST("is-alive should keep 'was an entity, now dead' meaning what it says")
  {
    // In one chain: the validity test becomes a comparison with an empty
    // handle; the renamed test alone would never hold.
    const auto chain = lspTest::analysed(
        "void f(sfs::Entity x, sfs::Entity y)\n{\n"
        "  if (x.isValid() && ready && !x.isAlive()) gone(x);\n"
        "  bool fine = !y.isValid() || y.isAlive();\n}\n");
    CHECK(lspTest::count(*chain, RuleId::IsAlive) == 2u);
    std::vector<sfs::codegen::TextEdit> all;
    for (const auto& d : chain->diagnostics)
      if (d.rule == RuleId::IsAlive)
        all.insert(all.end(), d.fix.begin(), d.fix.end());
    CHECK(lspTest::applyEdits(chain->text, all) ==
          "void f(sfs::Entity x, sfs::Entity y)\n{\n"
          "  if (x != sfs::Entity{} && ready && !x.isValid()) gone(x);\n"
          "  bool fine = y == sfs::Entity{} || y.isValid();\n}\n");
    // Under an earlier validity test (an if, or an early-out): reported for a
    // hand fix.
    const auto guarded = lspTest::analysed(
        "void f(sfs::Entity x, sfs::Entity y)\n{\n"
        "  if (x.isValid())\n    if (!x.isAlive()) gone(x);\n"
        "  if (!y.isValid())\n    return;\n  if (!y.isAlive()) gone(y);\n}\n");
    CHECK(lspTest::count(*guarded, RuleId::IsAlive) == 2u);
    for (const auto& d : guarded->diagnostics)
      if (d.rule == RuleId::IsAlive)
        CHECK(d.fix.empty() &&
              d.message.find("!= sfs::Entity{}") != std::string::npos);
  }

  TEST("row-lookup should flag lookups of a loop's own row and unpack it")
  {
    const auto query = lspTest::analysed(
        "void f()\n{\n"
        "  for (const sfs::Entity& v : registry->query<A, sfs::Any<B, C>>())\n"
        "  {\n    A& a = v.getComponent<A>();\n"
        "    a.n += v.getComponent<B>().k;\n  }\n}\n");
    CHECK(lspTest::count(*query, RuleId::RowLookup) == 2u);
    const auto* found = lspTest::first(*query, RuleId::RowLookup);
    CHECK(found && found->message ==
                       "This looks A up on the loop's own entity, whose row "
                       "already has it. Unpack the row instead, as in for "
                       "(auto [v, ...] : ...).");
    const auto queryFixed = fixed(*query, RuleId::RowLookup);
    CHECK(queryFixed->text ==
          "void f()\n{\n"
          "  for (auto [a, b, c] : "
          "registry->query<A, sfs::Any<B, C>>().components())\n"
          "  {\n    a.n += b->k;\n  }\n}\n");
    CHECK(queryFixed->diagnostics.empty());

    // The system's own list: only what's used, in registration order.
    const auto system = lspTest::analysed(
        "class S : public sfs::System\n{\n"
        "  void create() override\n  {\n    registerComponent<Leaf>();\n"
        "    registerComponent<sfs::TransformComponent>();\n  }\n"
        "  void update(double) override\n  {\n"
        "    for (const sfs::Entity& e : getEntities())\n"
        "      e.getComponent<sfs::TransformComponent>().x = 1;\n  }\n};\n");
    const auto systemFixed = fixed(*system, RuleId::RowLookup);
    CHECK(systemFixed->text.find(
              "    for (sfs::TransformComponent& transform : "
              "getEntities<sfs::TransformComponent>())\n      transform.x = "
              "1;\n") != std::string::npos);
    CHECK(lspTest::count(*systemFixed, RuleId::RowLookup) == 0u);

    // A query alias of the file, as written; an entity the body still reads
    // stays.
    const auto alias = lspTest::analysed(
        "using Parts = sfs::Query<Wheel, sfs::TransformComponent>;\n"
        "void f()\n{\n  for (sfs::Entity p : registry->query<Parts>())\n"
        "    p.getComponent<Wheel>().spin = p.getId();\n}\n");
    const auto aliasFixed = fixed(*alias, RuleId::RowLookup);
    CHECK(aliasFixed->text.find("for (auto [p, wheel, transform] : "
                                "registry->query<Parts>())\n    wheel.spin = "
                                "p.getId();") != std::string::npos);
    CHECK(aliasFixed->diagnostics.empty());
  }

  TEST("row-lookup should read an engine system's registration from its "
       "header and .cpp")
  {
    // The SFS tree has no project index: the class is found through the
    // .cpp's #include, under an include folder above it.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "sfsLspEngineTree";
    std::filesystem::remove_all(root);
    lspTest::writeFile(root / ".sfs-engine", "# SFS\n");
    lspTest::writeFile(
        root / "engine/include/engine/runtime/systems/animationSystem.h",
        "#pragma once\nnamespace sfs\n{\nclass AnimationSystem : public "
        "System\n"
        "{\nprotected:\n  void create() override { "
        "registerComponent<AnimatorComponent>(); }\n"
        "  void update(double) override;\n};\n"
        "class PhysicsSystem2D : public System\n{\nprotected:\n"
        "  void create() override;\n  void update(double) override;\n};\n}\n");
    const auto animation = lspTest::analysed(
        "#include \"engine/runtime/systems/animationSystem.h\"\nnamespace "
        "sfs\n{\nvoid AnimationSystem::update(double)\n{\n"
        "  for (const Entity& e : getEntities())\n"
        "    e.getComponent<AnimatorComponent>().time += 1;\n}\n}\n",
        root / "engine/src/runtime/animation/animationSystem.cpp");
    CHECK(lspTest::count(*animation, RuleId::RowLookup) == 1u);
    CHECK(fixed(*animation, RuleId::RowLookup)
              ->text.find("for (AnimatorComponent& animator : "
                          "getEntities<AnimatorComponent>())\n    "
                          "animator.time += 1;") != std::string::npos);
    // Registered in the .cpp's own out-of-line create().
    const auto physics = lspTest::analysed(
        "#include \"engine/runtime/systems/animationSystem.h\"\nnamespace "
        "sfs\n{\nvoid PhysicsSystem2D::create()\n{\n"
        "  registerComponent<Body2D>();\n  "
        "registerComponent<TransformComponent>();\n}\n"
        "void PhysicsSystem2D::update(double)\n{\n"
        "  for (const Entity& e : getEntities())\n"
        "    e.getComponent<Body2D>().v = 0;\n}\n}\n",
        root / "engine/src/runtime/systems/physicsSystem2D.cpp");
    CHECK(lspTest::count(*physics, RuleId::RowLookup) == 1u);
    CHECK(fixed(*physics, RuleId::RowLookup)
              ->text.find("for (Body2D& body2D : getEntities<Body2D>())") !=
          std::string::npos);
    std::filesystem::remove_all(root);
  }

  TEST("row-lookup should report loops it can't unpack without a fix")
  {
    // Over a bound list; the entity reassigned; a lookup in a lambda; a name
    // the loop already uses.
    const auto doc = lspTest::analysed(
        "void f()\n{\n"
        "  const std::vector<sfs::Entity>& list = registry->query<A>();\n"
        "  for (const sfs::Entity& v : list)\n    v.getComponent<A>().x = 1;\n"
        "  for (sfs::Entity v : registry->query<A>())\n  {\n"
        "    v.getComponent<A>().x = 1;\n    v = other();\n  }\n"
        "  for (const sfs::Entity& v : registry->query<A>())\n"
        "    later([&] { v.getComponent<A>().x = 1; });\n"
        "  for (const sfs::Entity& v : registry->query<A>())\n"
        "    v.getComponent<A>().x = a;\n}\n");
    CHECK(lspTest::count(*doc, RuleId::RowLookup) == 4u);
    for (const auto& d : doc->diagnostics)
      if (d.rule == RuleId::RowLookup)
        CHECK(d.fix.empty());
    // A copy of a list isn't its rows.
    CHECK(lspTest::count(*lspTest::analysed(
                             "void f()\n{\n  for (sfs::Entity v : "
                             "std::vector<sfs::Entity>(registry->query<A>()))\n"
                             "    v.getComponent<A>().x = 1;\n}\n"),
                         RuleId::RowLookup) == 0u);
  }

  TEST("filter-in-loop should flag a query loop that skips entities without "
       "a component, and add it to the query")
  {
    // A pointer declared in the condition becomes the row's binding.
    const auto declared = lspTest::analysed(
        "void f()\n{\n"
        "  for (const sfs::Entity& e : registry->query<A, B>())\n"
        "    if (Booking* booking = e.getComponent<Booking>())\n"
        "    {\n      booking->x = 1;\n      use(booking);\n"
        "      (*booking).y = 2;\n    }\n}\n");
    CHECK(lspTest::count(*declared, RuleId::FilterInLoop) == 1u);
    const auto* found = lspTest::first(*declared, RuleId::FilterInLoop);
    CHECK(found && found->message ==
                       "This loop goes through every entity in the query and "
                       "skips those without a Booking. Add Booking to the "
                       "query so it only visits the entities that have one.");
    const auto declaredFixed = fixed(*declared, RuleId::FilterInLoop);
    CHECK(declaredFixed->text ==
          "void f()\n{\n"
          "  for (auto [a, b, booking] : "
          "registry->query<A, B, Booking>().components())\n"
          "  {\n    booking.x = 1;\n    use(&booking);\n"
          "    (booking).y = 2;\n  }\n}\n");
    CHECK(declaredFixed->diagnostics.empty());

    // A loop that unpacks its row already gains a binding.
    const auto unpacked = lspTest::analysed(
        "void f()\n{\n  for (auto [e, a] : registry->query<A>())\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n"
        "      hitch->n += a.n;\n}\n");
    CHECK(lspTest::count(*unpacked, RuleId::FilterInLoop) == 1u);
    const auto unpackedFixed = fixed(*unpacked, RuleId::FilterInLoop);
    CHECK(unpackedFixed->text ==
          "void f()\n{\n  for (auto [a, hitch] : registry->query<A, "
          "Hitch>().components())\n    hitch.n += a.n;\n}\n");
    CHECK(unpackedFixed->diagnostics.empty());
    // The entity stays when what's left of the body reads it.
    const auto kept = fixed(
        *lspTest::analysed("void f()\n{\n  for (auto [e, a] : "
                           "registry->query<A>())\n"
                           "    if (Hitch* hitch = e.getComponent<Hitch>())\n"
                           "      hitch->n += a.n + e.getId();\n}\n"),
        RuleId::FilterInLoop);
    CHECK(kept->text == "void f()\n{\n  for (auto [e, a, hitch] : "
                        "registry->query<A, Hitch>())\n    hitch.n += a.n + "
                        "e.getId();\n}\n");
    CHECK(kept->diagnostics.empty());

    // A plain test in a block, as hasComponent: the list alone changes.
    const auto tested = lspTest::analysed(
        "void f()\n{\n  for (const sfs::Entity& e : registry->query<A>())\n"
        "  {\n    // Only the tagged ones.\n    if (e.hasComponent<Tag>())\n"
        "      count(e);\n  }\n}\n");
    CHECK(lspTest::count(*tested, RuleId::FilterInLoop) == 1u);
    // The comment would go: no fix.
    CHECK(lspTest::first(*tested, RuleId::FilterInLoop)->fix.empty());
    const auto plain = lspTest::analysed(
        "void f()\n{\n  for (const sfs::Entity& e : registry->query<A>())\n"
        "  {\n    if (e.getComponent<Tag>() != nullptr)\n"
        "      count(e);\n  }\n}\n");
    CHECK(fixed(*plain, RuleId::FilterInLoop)->text ==
          "void f()\n{\n  for (const sfs::Entity& e : registry->query<A, "
          "Tag>())\n  {\n    count(e);\n  }\n}\n");

    // Reported without a fix: over a bound list, the system's own list, a
    // condition with more to it, an entity by value used where a row's const
    // entity can't be, a pointer that's reassigned or passed on as const.
    const auto reported = lspTest::analysed(
        "class S : public sfs::System\n{\n"
        "  void create() override { registerComponent<Leaf>(); }\n"
        "  void update(double) override\n  {\n"
        "    const std::vector<sfs::Entity>& list = registry->query<A>();\n"
        "    for (const sfs::Entity& e : list)\n"
        "      if (const Booking* booking = e.getComponent<Booking>(); booking "
        "&& booking->x == y)\n        hit();\n"
        "    for (const sfs::Entity& e : getEntities())\n"
        "      if (Booking* booking = e.getComponent<Booking>())\n"
        "        booking->x = 1;\n"
        "    for (const sfs::Entity& e : registry->query<A>())\n"
        "      if (e.getComponent<Booking>() && ready)\n        go(e);\n"
        "    for (sfs::Entity e : registry->query<A>())\n"
        "      if (Booking* booking = e.getComponent<Booking>())\n"
        "        take(e);\n"
        "    for (const sfs::Entity& e : registry->query<A>())\n"
        "      if (Booking* booking = e.getComponent<Booking>())\n"
        "        booking = next(booking);\n"
        "    for (const sfs::Entity& e : registry->query<A>())\n"
        "      if (const Booking* booking = e.getComponent<Booking>())\n"
        "        show(booking);\n  }\n};\n");
    CHECK(lspTest::count(*reported, RuleId::FilterInLoop) == 6u);
    for (const auto& d : reported->diagnostics)
      if (d.rule == RuleId::FilterInLoop)
        CHECK(d.fix.empty());
    bool systemMessage = false;
    for (const auto& d : reported->diagnostics)
      systemMessage =
          systemMessage ||
          d.message == "This loop goes through every entity of the system and "
                       "skips those without a Booking. Go through a query "
                       "that also lists Booking so it only visits the "
                       "entities that have one.";
    CHECK(systemMessage);

    // Reported on the loop's `for`, so a comment beside or above the loop
    // hides it, with or without a reason.
    const std::string loop = "  for (auto [e, a] : registry->query<A>())";
    const std::string body = "    if (Hitch* hitch = e.getComponent<Hitch>())\n"
                             "      hitch->n = 1;\n}\n";
    CHECK(
        lspTest::first(*lspTest::analysed("void f()\n{\n" + loop + "\n" + body),
                       RuleId::FilterInLoop)
            ->range.startRow == 2u);
    for (const std::string& hidden :
         {"void f()\n{\n" + loop + " // sfs-ignore(filter-in-loop)\n" + body,
          "void f()\n{\n  // sfs-ignore-next-line(filter-in-loop)\n" + loop +
              "\n" + body,
          "void f()\n{\n  // sfs-ignore-next-line(filter-in-loop): the "
          "queries are gone by now, so, as it must, it walks them all.\n" +
              loop + "\n" + body})
      CHECK(lspTest::count(*lspTest::analysed(hidden), RuleId::FilterInLoop) ==
            0u);
  }

  TEST("filter-in-loop should leave loops that aren't a query's filter")
  {
    const auto doc = lspTest::analysed(
        "void f(const std::vector<sfs::Entity>& list, sfs::Entity other)\n{\n"
        // Not a query: a parameter, a braced list, another type's entities(),
        // a query view's own list.
        "  for (const sfs::Entity& e : list)\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n      hitch->n = 1;\n"
        "  for (const sfs::Entity& m : {a, b})\n"
        "    if (Hitch* hitch = m.getComponent<Hitch>())\n      hitch->n = 1;\n"
        "  for (const sfs::Entity& e : history->entities())\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n      hitch->n = 1;\n"
        "  for (const sfs::Entity& e : registry->query<A>().entities())\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n      hitch->n = 1;\n"
        // More than the if, or an else.
        "  for (const sfs::Entity& e : registry->query<A>())\n  {\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n      hitch->n = 1;\n"
        "    e.getComponent<A>()->x = 2;\n  }\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n      hitch->n = 1;\n"
        "    else\n      lost(e);\n"
        // A guaranteed component, not a filter.
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>(); sfs::expect(hitch, "
        "e))\n      hitch->n = 1;\n"
        // Another entity's component.
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (Hitch* hitch = other.getComponent<Hitch>())\n"
        "      hitch->n = 1;\n"
        "  for (auto [e, a] : registry->query<A>())\n"
        "    if (Hitch* hitch = a.target.getComponent<Hitch>())\n"
        "      hitch->n = 1;\n"
        // Runs when it's missing.
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (!e.hasComponent<Hitch>())\n      add(e);\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (const Hitch* hitch = e.getComponent<Hitch>(); !hitch || "
        "hitch->n)\n      add(e);\n"
        // Named by the query: required, excluded.
        "  for (const sfs::Entity& e : registry->query<A, sfs::Hitch>())\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n      hitch->n = 1;\n"
        "  for (const sfs::Entity& e : registry->query<A, "
        "sfs::Excluding<Hitch>>())\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n      hitch->n = 1;\n"
        // A list naming it couldn't be gone through as the body removes it.
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (Hitch* hitch = e.getComponent<Hitch>())\n"
        "      e.removeComponent<Hitch>();\n}\n");
    CHECK(lspTest::count(*doc, RuleId::FilterInLoop) == 0u);
  }

  TEST("unused-row-entity should flag a row's unread entity and loop over the "
       "components alone")
  {
    // One required component: the row converts to it, const as the term is.
    const auto one = lspTest::analysed(
        "void f()\n{\n"
        "  for (auto [e, booking] : registry->query<Booking>())\n"
        "    booking.wait += 1;\n}\n");
    CHECK(lspTest::count(*one, RuleId::UnusedRowEntity) == 1u);
    const auto* found = lspTest::first(*one, RuleId::UnusedRowEntity);
    CHECK(found &&
          found->message ==
              "This loop unpacks each row's entity but never uses it. Loop "
              "over the Booking alone: `Booking& booking`.");
    CHECK(found && found->range.startRow == 2u &&
          found->range.startColumn == 13u);
    const auto oneFixed = fixed(*one, RuleId::UnusedRowEntity);
    CHECK(oneFixed->text ==
          "void f()\n{\n"
          "  for (Booking& booking : registry->query<Booking>())\n"
          "    booking.wait += 1;\n}\n");
    CHECK(oneFixed->diagnostics.empty());
    CHECK(fixed(*lspTest::analysed(
                    "void f()\n{\n  for (const auto& [e, hitch] : "
                    "registry->query<const sfs::Hitch, "
                    "sfs::Excluding<Done>>())\n    use(hitch);\n}\n"),
                RuleId::UnusedRowEntity)
              ->text == "void f()\n{\n  for (const sfs::Hitch& hitch : "
                        "registry->query<const sfs::Hitch, "
                        "sfs::Excluding<Done>>())\n    use(hitch);\n}\n");
    // A typed hierarchy walk, and `_`, a name like any other.
    CHECK(fixed(*lspTest::analysed(
                    "void f(sfs::Entity root)\n{\n  for (auto&& [_, leaf] : "
                    "sfs::hierarchy::childrenOf<Leaf>(root))\n    "
                    "leaf.open = true;\n}\n"),
                RuleId::UnusedRowEntity)
              ->text == "void f(sfs::Entity root)\n{\n  for (Leaf& leaf : "
                        "sfs::hierarchy::childrenOf<Leaf>(root))\n    "
                        "leaf.open = true;\n}\n");

    // Several, or an Any member: the header drops the entity and the range
    // gains components().
    const auto several = lspTest::analysed(
        "void f()\n{\n"
        "  for (const auto& [e, motion, command] :\n"
        "       registry->query<Motion, DriveCommand>())\n"
        "    motion.speed = command.speed;\n"
        "  for (auto [e, a, b, c] : registry->query<A, sfs::Any<B, C>>())\n"
        "    a.n += b ? b->k : c->k;\n"
        "  for (auto [e, leaf, stem] : "
        "sfs::hierarchy::descendants<sfs::Any<Leaf, Stem>>(root))\n"
        "    leaf->open = !stem;\n}\n");
    CHECK(lspTest::count(*several, RuleId::UnusedRowEntity) == 3u);
    CHECK(lspTest::first(*several, RuleId::UnusedRowEntity)->message ==
          "This loop unpacks each row's entity but never uses it. Loop over "
          "the components alone, with `.components()`.");
    std::string text = several->text;
    for (int i = 0; i < 3; ++i)
    {
      const auto now = lspTest::analysed(text);
      const auto* next = lspTest::first(*now, RuleId::UnusedRowEntity);
      CHECK(next && !next->fix.empty());
      if (next)
        text = lspTest::applyEdits(text, next->fix);
    }
    CHECK(text ==
          "void f()\n{\n"
          "  for (const auto& [motion, command] :\n"
          "       registry->query<Motion, DriveCommand>().components())\n"
          "    motion.speed = command.speed;\n"
          "  for (auto [a, b, c] : registry->query<A, sfs::Any<B, "
          "C>>().components())\n"
          "    a.n += b ? b->k : c->k;\n"
          "  for (auto [leaf, stem] : "
          "sfs::hierarchy::descendants<sfs::Any<Leaf, Stem>>(root)"
          ".components())\n"
          "    leaf->open = !stem;\n}\n");
    CHECK(lspTest::analysed(text)->diagnostics.empty());

    // A variable holding the view takes components(); a bound list, a
    // helper's view or the system's own list doesn't, so no fix.
    CHECK(
        fixed(*lspTest::analysed(
                  "void f()\n{\n  const auto list = registry->query<A, "
                  "B>();\n  for (auto [e, a, b] : list)\n    a.n = b.n;\n}\n"),
              RuleId::UnusedRowEntity)
            ->text == "void f()\n{\n  const auto list = registry->query<A, "
                      "B>();\n  for (auto [a, b] : list.components())\n    "
                      "a.n = b.n;\n}\n");
    const auto bound = lspTest::analysed(
        "void f()\n{\n"
        "  const std::vector<sfs::Entity>& list = registry->query<A, B>();\n"
        "  for (auto [e, a, b] : list)\n    a.n = b.n;\n}\n");
    CHECK(lspTest::count(*bound, RuleId::UnusedRowEntity) == 1u);
    CHECK(lspTest::first(*bound, RuleId::UnusedRowEntity)->fix.empty());
  }

  TEST("unused-row-entity should leave loops that read the entity or don't "
       "unpack a row")
  {
    for (const std::string body :
         {"    e.getComponent<B>();\n",
          "    later([&] { take(e); });\n",
          "    LOG_INFO(\"at \" + std::to_string(e.getId()));\n",
          "    n += sizeof(e);\n",
          "    decltype(e) other = e;\n",
          "    if (a.owner == e)\n      a.n = 1;\n"})
      CHECK(
          lspTest::count(*lspTest::analysed("void f()\n{\n  for (auto [e, a] : "
                                            "registry->query<A>())\n" +
                                            body + "}\n"),
                         RuleId::UnusedRowEntity) == 0u);
    // A member named like the entity isn't it.
    CHECK(lspTest::count(
              *lspTest::analysed("void f()\n{\n  for (auto [e, a] : "
                                 "registry->query<A>())\n    a.e = 1;\n}\n"),
              RuleId::UnusedRowEntity) == 1u);
    // Not unpacked, not a row, or not the row's shape.
    for (const std::string loop :
         {"  for (const sfs::Entity& e : registry->query<A>())\n",
          "  for (auto [e, a] : pairs)\n",
          "  for (auto [e, a, b] : registry->query<A>())\n",
          "  for (auto [e] : registry->query<sfs::Excluding<A>>())\n"})
      CHECK(lspTest::count(
                *lspTest::analysed("void f()\n{\n" + loop + "    go();\n}\n"),
                RuleId::UnusedRowEntity) == 0u);

    // Reported on the entity's name, so a comment beside or above hides it.
    const std::string loop = "  for (auto [e, a] : registry->query<A>())";
    const std::string body = "    a.n = 1;\n}\n";
    for (const std::string& hidden :
         {"void f()\n{\n" + loop + " // sfs-ignore(unused-row-entity)\n" + body,
          "void f()\n{\n  // sfs-ignore-next-line(unused-row-entity): the "
          "entity goes in a later change.\n" +
              loop + "\n" + body})
      CHECK(lspTest::count(
                *lspTest::analysed(hidden), RuleId::UnusedRowEntity) == 0u);
  }

  TEST("row-component-copy should read a copied component in place, and "
       "leave a written copy to the author")
  {
    // Only read: the fix reads it by const reference.
    const auto read =
        lspTest::analysed("void f()\n{\n"
                          "  for (Pos pos : registry->query<Pos>())\n"
                          "    total += pos.x * 2 + (pos.ready ? 1 : 0);\n}\n");
    const auto* found = lspTest::first(*read, RuleId::RowComponentCopy);
    CHECK(found && found->message ==
                       "This loop copies each Pos but only reads it. Read it "
                       "in place with const Pos&.");
    const auto readFixed = fixed(*read, RuleId::RowComponentCopy);
    CHECK(readFixed->text ==
          "void f()\n{\n"
          "  for (const Pos& pos : registry->query<Pos>())\n"
          "    total += pos.x * 2 + (pos.ready ? 1 : 0);\n}\n");
    CHECK(readFixed->diagnostics.empty());
    // A const copy reads whatever it's passed to; a walk's rows convert too,
    // and a const term reads.
    const auto constant = lspTest::analysed(
        "void f(sfs::Entity root)\n{\n"
        "  for (const Leaf leaf : sfs::hierarchy::childrenOf<Leaf>(root))\n"
        "    show(leaf);\n"
        "  for (Leaf leaf : registry->query<const Leaf>())\n"
        "    total += leaf.size;\n}\n");
    CHECK(lspTest::count(*constant, RuleId::RowComponentCopy) == 2u);
    std::string text = constant->text;
    for (int i = 0; i < 2; ++i)
    {
      const auto now = lspTest::analysed(text);
      if (const auto* next = lspTest::first(*now, RuleId::RowComponentCopy))
        text = lspTest::applyEdits(text, next->fix);
    }
    CHECK(text ==
          "void f(sfs::Entity root)\n{\n"
          "  for (const Leaf& leaf : sfs::hierarchy::childrenOf<Leaf>(root))\n"
          "    show(leaf);\n"
          "  for (const Leaf& leaf : registry->query<const Leaf>())\n"
          "    total += leaf.size;\n}\n");

    // Written, maybe as scratch: reported without a fix; a const term's
    // written copy isn't reported, as its rows can't be written.
    for (const std::string body : {"    pos.x += 10;\n",
                                   "    ++pos.x;\n",
                                   "    pos.items.push_back(1);\n",
                                   "    take(std::move(pos));\n",
                                   "    bump(pos);\n",
                                   "    stream >> pos.x;\n",
                                   "    (ready ? pos : other).x = 1;\n"})
    {
      const auto written = lspTest::analysed(
          "void f()\n{\n  for (Pos pos : registry->query<Pos>())\n" + body +
          "}\n");
      const auto* copy = lspTest::first(*written, RuleId::RowComponentCopy);
      CHECK(copy && copy->fix.empty() &&
            copy->message ==
                "This loop copies each Pos, so changes to the copy don't "
                "reach the component. If the copy is scratch, keep it; "
                "otherwise use Pos&.");
      CHECK(
          lspTest::count(*lspTest::analysed("void f()\n{\n  for (Pos pos : "
                                            "registry->query<const Pos>())\n" +
                                            body + "}\n"),
                         RuleId::RowComponentCopy) == 0u);
    }

    // Not a copy of the row's component: a reference, the row itself (auto),
    // the entity, another type, a row of several components, or a vector of
    // the list's entities.
    for (const std::string loop :
         {"  for (Pos& pos : registry->query<Pos>())\n",
          "  for (auto pos : registry->query<Pos>())\n",
          "  for (sfs::Entity pos : registry->query<Pos>())\n",
          "  for (Other pos : registry->query<Pos>())\n",
          "  for (Pos pos : registry->query<Pos, Hitch>())\n",
          "  for (Pos pos : registry->query<sfs::Any<Pos, Hitch>>())\n",
          "  const std::vector<sfs::Entity>& list = registry->query<Pos>();\n"
          "  for (Pos pos : list)\n"})
      CHECK(lspTest::count(*lspTest::analysed("void f()\n{\n" + loop +
                                              "    total += pos.x;\n}\n"),
                           RuleId::RowComponentCopy) == 0u);
  }

  TEST("filter-in-loop and row-lookup should follow helpers that return a "
       "query")
  {
    const auto doc = lspTest::analysed(
        "class S : public sfs::System\n{\n"
        "  const std::vector<sfs::Entity>& vehicles();\n"
        "  const std::vector<sfs::Entity>& tools() { return "
        "registry->query<Tool>(); }\n"
        "  void update(double) override;\n};\n"
        "const std::vector<sfs::Entity>& S::vehicles()\n{\n"
        "  return registry->query<A, B>();\n}\n"
        "void S::update(double)\n{\n"
        "  for (const sfs::Entity& v : vehicles())\n"
        "    if (Booking* booking = v.getComponent<Booking>())\n"
        "      booking->x = 1;\n"
        "  for (const sfs::Entity& v : this->tools())\n"
        "    v.getComponent<Tool>()->x = 1;\n}\n");
    CHECK(lspTest::count(*doc, RuleId::FilterInLoop) == 1u);
    CHECK(lspTest::count(*doc, RuleId::RowLookup) == 1u);
    for (const auto& d : doc->diagnostics)
      CHECK(d.fix.empty());
    // A free helper, outside any class.
    CHECK(lspTest::count(
              *lspTest::analysed(
                  "const std::vector<sfs::Entity>& parts()\n{\n"
                  "  return registry->query<Part>();\n}\nvoid f()\n{\n"
                  "  for (const sfs::Entity& p : parts())\n"
                  "    if (p.hasComponent<Tag>())\n      count(p);\n}\n"),
              RuleId::FilterInLoop) == 1u);
    // Not a helper's query: a filtered or copied list, more than a return,
    // parameters, a free function the class may hide with its own.
    const auto others = lspTest::analysed(
        "const std::vector<sfs::Entity>& near() { return "
        "registry->query<A>(); }\n"
        "class S : public sfs::System\n{\n"
        "  std::vector<sfs::Entity> runs() { return "
        "listed(registry->query<A>()); }\n"
        "  std::vector<sfs::Entity> copy() { return registry->query<A>(); }\n"
        "  const std::vector<sfs::Entity>& logged()\n  {\n"
        "    log();\n    return registry->query<A>();\n  }\n"
        "  const std::vector<sfs::Entity>& some(int) { return "
        "registry->query<A>(); }\n"
        "  void update(double) override\n  {\n"
        "    for (const sfs::Entity& e : runs())\n"
        "      if (e.hasComponent<Tag>())\n        count(e);\n"
        "    for (const sfs::Entity& e : copy())\n"
        "      if (e.hasComponent<Tag>())\n        count(e);\n"
        "    for (const sfs::Entity& e : logged())\n"
        "      if (e.hasComponent<Tag>())\n        count(e);\n"
        "    for (const sfs::Entity& e : some())\n"
        "      if (e.hasComponent<Tag>())\n        count(e);\n"
        "    for (const sfs::Entity& e : near())\n"
        "      if (e.hasComponent<Tag>())\n        count(e);\n  }\n"
        "  const std::vector<sfs::Entity>& near();\n};\n");
    CHECK(lspTest::count(*others, RuleId::FilterInLoop) == 0u);
  }

  TEST("filter-in-loop should flag walks of every entity, and loops that "
       "skip with continue")
  {
    // entities(): the one-if form, components as && operands (negated ones
    // aren't a filter it suggests).
    const auto every = lspTest::analysed(
        "void f(sfs::Entity flatbed)\n{\n"
        "  for (const sfs::Entity& part : registry->entities())\n"
        "    if (part != flatbed && part.hasComponent<sfs::NameComponent>() "
        "&&\n        part.hasComponent<sfs::TransformComponent>() &&\n"
        "        !part.hasComponent<Implement>())\n"
        "      parts.push_back(part);\n"
        "  for (const sfs::Entity& mesh : scene().entities())\n"
        "    if (mesh.hasComponent<sfs::MeshRendererComponent>() && mesh == "
        "flatbed)\n      glass.push_back(mesh);\n}\n");
    CHECK(lspTest::count(*every, RuleId::FilterInLoop) == 2u);
    const auto* found = lspTest::first(*every, RuleId::FilterInLoop);
    CHECK(found && found->fix.empty() &&
          found->message ==
              "This loop goes through every entity and skips those without a "
              "sfs::NameComponent or a sfs::TransformComponent. Go through a "
              "query that lists sfs::NameComponent and sfs::TransformComponent "
              "instead; a query leaves out entities created this frame and "
              "not yet added, so check the loop doesn't need those.");

    // Lookups, then `if (...) continue;`: over every entity and over a query.
    const auto skipping = lspTest::analysed(
        "void f(sfs::Entity machine)\n{\n"
        "  for (const sfs::Entity& part : registry->entities())\n  {\n"
        "    if (!part.hasComponent<Name>() || "
        "!part.hasComponent<Transform>())\n      continue;\n"
        "    use(part);\n  }\n"
        "  for (const sfs::Entity& part : registry->query<A>())\n  {\n"
        "    const Name* named = part.getComponent<Name>();\n"
        "    if (!named || part.getComponent<Transform>() == nullptr ||\n"
        "        !owns(machine, part))\n    {\n      continue;\n    }\n"
        "    use(named->name);\n  }\n}\n");
    CHECK(lspTest::count(*skipping, RuleId::FilterInLoop) == 2u);
    bool queryMessage = false;
    for (const auto& d : skipping->diagnostics)
      queryMessage =
          queryMessage ||
          (d.fix.empty() &&
           d.message == "This loop goes through every entity in the query and "
                        "skips those without a Name or a Transform. Add Name "
                        "and Transform to the query so it only visits the "
                        "entities that have them.");
    CHECK(queryMessage);

    // Not a filter: something before the test, a return or break instead of
    // continue, skipping those that have it, a component the query names, or
    // the test with an else.
    const auto others = lspTest::analysed(
        "void f()\n{\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n  {\n"
        "    ++seen;\n    if (!e.hasComponent<Name>())\n      continue;\n"
        "    use(e);\n  }\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n  {\n"
        "    if (!e.hasComponent<Name>())\n      return;\n    use(e);\n  }\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n  {\n"
        "    if (!e.hasComponent<Name>())\n      break;\n    use(e);\n  }\n"
        "  for (const sfs::Entity& e : registry->entities())\n  {\n"
        "    if (e.hasComponent<Hidden>())\n      continue;\n    use(e);\n  }\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n  {\n"
        "    const A* a = e.getComponent<A>();\n    if (!a)\n      continue;\n"
        "    use(a);\n  }\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n  {\n"
        "    if (!e.hasComponent<Name>())\n      continue;\n"
        "    else\n      use(e);\n  }\n}\n");
    CHECK(lspTest::count(*others, RuleId::FilterInLoop) == 0u);
  }

  TEST("row-lookup and filter-in-loop should read typed hierarchy walks as "
       "a query's rows")
  {
    const auto lookup = lspTest::analysed(
        "void f(sfs::Entity root)\n{\n"
        "  for (const sfs::Entity& c : sfs::hierarchy::childrenOf<A>(root))\n"
        "    c.getComponent<A>()->x = 1;\n}\n");
    CHECK(fixed(*lookup, RuleId::RowLookup)->text ==
          "void f(sfs::Entity root)\n{\n"
          "  for (A& a : sfs::hierarchy::childrenOf<A>(root))\n"
          "    a.x = 1;\n}\n");
    const auto filter = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n"
        "  for (auto [part, named] : sfs::hierarchy::descendants<Name>(e))\n"
        "    if (Tag* tag = part.getComponent<Tag>())\n      tag->n = 1;\n}\n");
    const auto* found = lspTest::first(*filter, RuleId::FilterInLoop);
    CHECK(found && found->message ==
                       "This loop goes through every entity in the walk and "
                       "skips those without a Tag. Add Tag to the walk's terms "
                       "so it only visits the entities that have one.");
    CHECK(fixed(*filter, RuleId::FilterInLoop)->text ==
          "void f(sfs::Entity e)\n{\n"
          "  for (auto [named, tag] : sfs::hierarchy::descendants<Name, "
          "Tag>(e).components())\n    tag.n = 1;\n}\n");
    // A negated test isn't a filter it suggests; an untyped walk isn't rows.
    CHECK(
        lspTest::count(*lspTest::analysed(
                           "void f(sfs::Entity e)\n{\n"
                           "  for (auto [part, named, transform] : "
                           "sfs::hierarchy::descendants<sfs::NameComponent, "
                           "sfs::TransformComponent>(e))\n"
                           "    if (!part.hasComponent<X>() && named.ok)\n"
                           "      use(part);\n"
                           "  for (const sfs::Entity& c : "
                           "sfs::hierarchy::childrenOf(e))\n"
                           "    if (c.hasComponent<X>())\n      use(c);\n}\n"),
                       RuleId::FilterInLoop) == 0u);
  }

  TEST("filter-in-loop should read a typed system list as its registration")
  {
    // Every entity of the system has B: no filter, but B is in the row.
    const auto doc = lspTest::analysed(
        "class S : public sfs::System\n{\n"
        "  void create() override\n  {\n    registerComponent<A>();\n"
        "    registerComponent<B>();\n  }\n"
        "  void update(double) override\n  {\n"
        "    for (auto [e, a] : getEntities<A>())\n"
        "      if (B* b = e.getComponent<B>())\n        b->x = a.x;\n"
        "    for (const sfs::Entity& e : getEntities<A>())\n"
        "      e.getComponent<B>()->x = 1;\n  }\n};\n");
    CHECK(lspTest::count(*doc, RuleId::FilterInLoop) == 0u);
    CHECK(lspTest::count(*doc, RuleId::RowLookup) == 1u);
    CHECK(fixed(*doc, RuleId::RowLookup)
              ->text.find("    for (auto [a, b] : getEntities<A, "
                          "B>().components())\n      b.x = 1;\n") !=
          std::string::npos);
    // A typed list whose system can't be read may have more: not reported.
    CHECK(lspTest::count(
              *lspTest::analysed(
                  "void Elsewhere::update(double)\n{\n"
                  "  for (const sfs::Entity& e : getEntities<A>())\n"
                  "    if (e.hasComponent<B>())\n      count(e);\n}\n"),
              RuleId::FilterInLoop) == 0u);
  }

  TEST("filter-in-loop should leave lookups where a component being removed "
       "is still found")
  {
    const auto doc = lspTest::analysed(
        "class S : public sfs::System\n{\n"
        "  void create() override\n  {\n    registerComponent<A>();\n"
        "    observe<Leaf, sfs::Excluding<Hidden>>(\n"
        "        [this](const sfs::Entity&)\n        {\n"
        "          for (const sfs::Entity& e : registry->query<A>())\n"
        "            if (Hidden* hidden = e.getComponent<Hidden>())\n"
        "              hidden->n = 1;\n        },\n"
        "        [](const sfs::Entity&) {});\n  }\n"
        "  void onEntityRemoved(sfs::Entity) override\n  {\n"
        "    for (const sfs::Entity& e : registry->query<A>())\n"
        "      if (Hitch* hitch = e.getComponent<Hitch>())\n"
        "        hitch->n = 1;\n"
        "    for (const sfs::Entity& e : registry->query<A>())\n"
        "      if (e.hasComponent<Tag>())\n        count(e);\n  }\n};\n");
    // Only the hasComponent test, which reads the component's bit.
    CHECK(lspTest::count(*doc, RuleId::FilterInLoop) == 1u);
    const auto* found = lspTest::first(*doc, RuleId::FilterInLoop);
    CHECK(found && found->message.find("without a Tag") != std::string::npos &&
          !found->fix.empty());
  }

  TEST("filter-in-loop and row-lookup should read only lists that are a "
       "query's rows")
  {
    // A copy, a view assigned again, two declarators, a view passed on:
    // not the query's rows.
    const auto copies = lspTest::analysed(
        "void f(sfs::Entity x)\n{\n"
        "  std::vector<sfs::Entity> list = registry->query<A>();\n"
        "  list.push_back(x);\n"
        "  for (const sfs::Entity& e : list)\n"
        "    if (e.hasComponent<Tag>())\n      count(e);\n"
        "  auto view = registry->query<A>();\n"
        "  view = registry->query<B>();\n"
        "  for (const sfs::Entity& e : view)\n"
        "    e.getComponent<A>()->x = 1;\n"
        "  auto one = registry->query<A>(), two = registry->query<B>();\n"
        "  for (const sfs::Entity& e : two)\n"
        "    e.getComponent<A>()->x = 1;\n"
        "  auto passed = registry->query<A>();\n  reset(passed);\n"
        "  for (const sfs::Entity& e : passed)\n"
        "    e.getComponent<A>()->x = 1;\n}\n");
    CHECK(lspTest::count(*copies, RuleId::FilterInLoop) == 0u);
    CHECK(lspTest::count(*copies, RuleId::RowLookup) == 0u);
    // A const view and a view never changed are, and the list being shared
    // is advised against changing.
    const auto views =
        lspTest::analysed("void f()\n{\n"
                          "  const auto& list = registry->query<A>();\n"
                          "  for (const sfs::Entity& e : list)\n"
                          "    if (e.hasComponent<Tag>())\n      count(e);\n"
                          "  auto view = registry->query<A>();\n"
                          "  for (const sfs::Entity& e : view)\n"
                          "    e.getComponent<A>()->x = 1;\n}\n");
    const auto* shared = lspTest::first(*views, RuleId::FilterInLoop);
    CHECK(shared && shared->fix.empty() &&
          shared->message ==
              "This loop goes through every entity in the query and skips "
              "those without a Tag. Go through a query that also lists Tag so "
              "it only visits the entities that have one.");
    CHECK(lspTest::count(*views, RuleId::RowLookup) == 1u);
    // entities() of a name ending in registry or scene, not merely holding it.
    const auto receivers = lspTest::analysed(
        "void f()\n{\n"
        "  for (const sfs::Entity& e : sceneQuery.entities())\n"
        "    if (e.hasComponent<Tag>())\n      count(e);\n"
        "  for (const sfs::Entity& e : registryView.entities())\n"
        "    if (e.hasComponent<Tag>())\n      count(e);\n"
        "  for (const sfs::Entity& e : m_Registry.entities())\n"
        "    if (e.hasComponent<Tag>())\n      count(e);\n}\n");
    CHECK(lspTest::count(*receivers, RuleId::FilterInLoop) == 1u);
  }

  TEST("filter-in-loop's fix should keep what it can't rewrite exactly, or "
       "give none")
  {
    // An empty term list gains the term alone.
    const auto empty = lspTest::analysed(
        "void f()\n{\n  for (const sfs::Entity& e : registry->query<>())\n"
        "    if (e.hasComponent<Tag>())\n      count(e);\n}\n");
    CHECK(fixed(*empty, RuleId::FilterInLoop)->text ==
          "void f()\n{\n  for (const sfs::Entity& e : registry->query<Tag>())\n"
          "    count(e);\n}\n");
    // A range-for's init-statement stays, here and in row-lookup's fix.
    const auto init = lspTest::analysed(
        "void f()\n{\n"
        "  for (int n = 0; const sfs::Entity& e : registry->query<A>())\n"
        "    if (Tag* tag = e.getComponent<Tag>())\n      tag->n = n;\n"
        "  for (int n = 0; const sfs::Entity& e : registry->query<A>())\n"
        "    e.getComponent<A>()->n = n;\n}\n");
    CHECK(fixed(*init, RuleId::FilterInLoop)
              ->text.find("  for (int n = 0; auto [a, tag] : "
                          "registry->query<A, Tag>().components())\n    "
                          "tag.n = n;\n") != std::string::npos);
    CHECK(fixed(*init, RuleId::RowLookup)
              ->text.find("  for (int n = 0; A& a : "
                          "registry->query<A>())\n    a.n = n;\n") !=
          std::string::npos);
    // Re-indenting would change a raw string or a continued line: no fix.
    const auto verbatim = lspTest::analysed(
        "void f()\n{\n  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (e.hasComponent<Tag>())\n"
        "      show(R\"(line one\n      line two)\");\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (e.hasComponent<Tag>())\n"
        "      show(\"one \\\n      two\");\n}\n");
    CHECK(lspTest::count(*verbatim, RuleId::FilterInLoop) == 2u);
    for (const auto& d : verbatim->diagnostics)
      if (d.rule == RuleId::FilterInLoop)
        CHECK(d.fix.empty());
  }

  TEST("filter-in-loop should leave tests that come after something with an "
       "effect")
  {
    const auto doc = lspTest::analysed(
        "void f()\n{\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (counter++ < 5 && e.hasComponent<Tag>())\n      count(e);\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n  {\n"
        "    if (tick() || !e.hasComponent<Tag>())\n      continue;\n"
        "    count(e);\n  }\n"
        // After the test, an effect runs only where the component is.
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (e.hasComponent<Tag>() && counter++ < 5)\n      count(e);\n"
        "  for (const sfs::Entity& e : registry->query<A>())\n"
        "    if (e.isValid() && e.hasComponent<Tag>())\n      count(e);\n}\n");
    CHECK(lspTest::count(*doc, RuleId::FilterInLoop) == 2u);
  }

  TEST("the entity rules should survive bodiless helpers and code being typed")
  {
    // Helpers with no body: deleted, defaulted, mid-typing.
    for (const std::string& helper :
         {std::string("parts() = delete;"),
          std::string("parts() = default;"),
          std::string("parts() const = delete;"),
          std::string("parts() override = default;"),
          std::string("parts() = default")})
      CHECK(lspTest::count(
                *lspTest::analysed(
                    "class S : public sfs::System\n{\n"
                    "  const std::vector<sfs::Entity>& " +
                    helper +
                    "\n  void update(double) override\n  {\n"
                    "    for (const sfs::Entity& e : parts())\n"
                    "      if (e.hasComponent<Tag>())\n        count(e);\n"
                    "  }\n};\n"),
                RuleId::FilterInLoop) == 0u);
    // Every prefix of these, and each with one character gone, as an editor
    // sees them while they're typed.
    const std::vector<std::string> sources = {
        "class S : public sfs::System\n{\n"
        "  const std::vector<sfs::Entity>& parts() { return "
        "registry->query<A>(); }\n"
        "  void create() override { registerComponent<A>(); "
        "observe<B, sfs::Excluding<C>>([](const sfs::Entity&) {}, "
        "[](const sfs::Entity&) {}); }\n"
        "  void update(double) override\n  {\n"
        "    for (int n = 0; auto [e, a] : getEntities<A>())\n"
        "      if (T* t = e.getComponent<T>(); t && t->x)\n"
        "        t->y = a.y;\n"
        "    for (const sfs::Entity& e : parts())\n    {\n"
        "      const Name* named = e.getComponent<Name>();\n"
        "      if (!named || !e.hasComponent<T>())\n        continue;\n"
        "      use(named->name);\n    }\n  }\n};\n",
        "void f(sfs::Entity root)\n{\n"
        "  const auto& list = registry->query<A, sfs::Any<B, C>>();\n"
        "  for (auto [p, n] : sfs::hierarchy::descendants<Name>(root))\n"
        "    if (p.hasComponent<X>() && p.getComponent<Y>() != nullptr)\n"
        "      p.getComponent<Name>()->x = 1;\n"
        "  for (const sfs::Entity& e : registry->entities())\n"
        "    if (Tag* tag = e.getComponent<Tag>())\n    {\n"
        "      tag->n = R\"(raw)\";\n    }\n"
        "  for (const sfs::Entity& e : list)\n"
        "    if (e.hasComponent<T>())\n      e.getComponent<T>()->x = 1;\n"
        "  for (A a : registry->query<A>())\n    n += a.x;\n"
        "  for (auto [a, b, c] : list.components())\n    n += a.x;\n"
        "}\n"};
    std::size_t runs = 0;
    for (const std::string& source : sources)
      for (std::size_t cut = 0; cut <= source.size(); ++cut)
      {
        lspTest::analysed(source.substr(0, cut));
        std::string missing = source;
        if (cut < source.size())
          missing.erase(cut, 1);
        lspTest::analysed(missing);
        runs += 2;
      }
    CHECK(runs > 1000u);
  }

  TEST("row-lookup and the term lints should read the project's query aliases")
  {
    Project project;
    lspTest::writeFile(
        project.root / "scripts/components/Wheel.h",
        "#pragma once\nstruct Wheel { float spin; };\nusing WheelParts = "
        "sfs::Query<Wheel, sfs::TransformComponent>;\n");
    project.index.rescan();
    project.index.indexAll(lspTest::parser());
    CHECK(project.index.findAlias("", "WheelParts") != nullptr);
    const auto doc = lspTest::analysed(
        "void f()\n{\n  for (const sfs::Entity& p : "
        "registry->query<WheelParts>())\n    p.getComponent<Wheel>().spin = "
        "0;\n  registry->query<WheelParts, Wheel>();\n}\n",
        project.root / "scripts/systems/Spin.cpp",
        &project.index);
    CHECK(lspTest::count(*doc, RuleId::RowLookup) == 1u);
    // Wheel is in the alias already: reported where the list names it, with
    // the repeat removable.
    CHECK(lspTest::count(*doc, RuleId::DuplicateTerm) == 1u);
  }

  TEST("setup-in-constructor should move an in-class constructor's set-up to "
       "create()")
  {
    const auto doc = lspTest::analysed(
        "namespace sfs\n{\nclass CameraSystem : public System\n{\npublic:\n"
        "  // Its components.\n  CameraSystem()\n  {\n"
        "    registerComponent<TransformComponent>();\n"
        "    // The player's view: real time.\n"
        "    setTimeStep(TimeStep::Real);\n  }\n\n"
        "  void update(double) override {}\n};\n}\n",
        "/t/cameraSystem.h");
    CHECK(lspTest::count(*doc, RuleId::SetupInConstructor) == 2u);
    const auto* found = lspTest::first(*doc, RuleId::SetupInConstructor);
    CHECK(found && found->message ==
                       "A system should not set itself up in its "
                       "constructor. Move its registerComponent, observe and "
                       "setTimeStep calls to create(), which runs once when "
                       "the system is added.");
    const auto after = fixed(*doc, RuleId::SetupInConstructor);
    CHECK(after->text ==
          "namespace sfs\n{\nclass CameraSystem : public System\n{\npublic:\n"
          "  void update(double) override {}\n\nprotected:\n"
          "  void create() override\n  {\n"
          "    registerComponent<TransformComponent>();\n"
          "    // The player's view: real time.\n"
          "    setTimeStep(TimeStep::Real);\n  }\n};\n}\n");
    CHECK(after->diagnostics.empty());

    // Into an existing create(), first; a constructor with initialisers stays.
    const auto existing = lspTest::analysed(
        "class S : public sfs::System\n{\n  int m_n;\n\npublic:\n"
        "  S() : m_n(1)\n  {\n    registerComponent<A>();\n  }\n\n"
        "protected:\n  void create() override\n  {\n"
        "    registerComponent<B>();\n  }\n};\n",
        "/t/S.h");
    CHECK(
        fixed(*existing, RuleId::SetupInConstructor)->text ==
        "class S : public sfs::System\n{\n  int m_n;\n\npublic:\n"
        "  S() : m_n(1)\n  {\n  }\n\n"
        "protected:\n  void create() override\n  {\n"
        "    registerComponent<A>();\n    registerComponent<B>();\n  }\n};\n");

    // A nested call, a class template, a parameter used: reported only.
    const auto refused = lspTest::analysed(
        "class N : public sfs::System { N() { if (on) registerComponent<A>(); "
        "} };\n"
        "template <typename T> class Q : public sfs::System { Q() { "
        "registerComponent<T>(); } };\n"
        "class P : public sfs::System { public: P(TimeStep s) { "
        "setTimeStep(s); } };\n"
        "class Plain { Plain() { registerComponent<A>(); } };\n",
        "/t/Refused.h");
    CHECK(lspTest::count(*refused, RuleId::SetupInConstructor) == 3u);
    for (const auto& d : refused->diagnostics)
      if (d.rule == RuleId::SetupInConstructor)
        CHECK(d.fix.empty());
  }

  TEST("setup-in-constructor should fix a header and its .cpp together")
  {
    Project project;
    const std::filesystem::path header =
        project.root / "scripts/systems/GateSystem.h";
    const std::filesystem::path source =
        project.root / "scripts/systems/GateSystem.cpp";
    const std::string headerText =
        "#pragma once\nclass GateSystem : public sfs::System\n{\npublic:\n"
        "  // Sets it up.\n  GateSystem();\n\nprivate:\n"
        "  void update(double deltaTime) override;\n};\n";
    lspTest::writeFile(header, headerText);
    project.index.rescan();
    project.index.indexAll(lspTest::parser());
    const std::string sourceText =
        "#include \"GateSystem.h\"\n\nGateSystem::GateSystem()\n{\n"
        "  registerComponent<GateLeaf>(); // its leaves\n"
        "  setTimeStep(TimeStep::Frame);\n}\n\n"
        "void GateSystem::update(double) {}\n";
    const auto doc = lspTest::analysed(sourceText, source, &project.index);
    CHECK(lspTest::count(*doc, RuleId::SetupInConstructor) == 2u);
    const auto* found = lspTest::first(*doc, RuleId::SetupInConstructor);
    CHECK(found != nullptr);
    if (!found)
      continue;
    std::vector<sfs::codegen::TextEdit> inSource, inHeader;
    for (const sfs::codegen::TextEdit& edit : found->fix)
      (edit.file.empty() ? inSource : inHeader).push_back(edit);
    CHECK(!inHeader.empty() && inHeader.front().file == header);
    CHECK(lspTest::applyEdits(sourceText, inSource) ==
          "#include \"GateSystem.h\"\n\nvoid GateSystem::create()\n{\n"
          "  registerComponent<GateLeaf>(); // its leaves\n"
          "  setTimeStep(TimeStep::Frame);\n}\n\n"
          "void GateSystem::update(double) {}\n");
    CHECK(lspTest::applyEdits(headerText, inHeader) ==
          "#pragma once\nclass GateSystem : public sfs::System\n{\n"
          "protected:\n  void create() override;\n\nprivate:\n"
          "  void update(double deltaTime) override;\n};\n");
  }

  TEST("component-pointer should flag the old reference forms and fix them")
  {
    const auto doc = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n  e.getComponent<A>().x = 1;\n"
        "  use(&registry->getComponent<A>(e));\n"
        "  auto& b = e.getComponent<B>();\n  b.y = 2;\n  take(b);\n"
        "  push(&b);\n  b.list[0] = 1;\n  C c = e.getComponent<C>();\n"
        "  e.getComponent<A>()->x = 1;\n  A* a = e.getComponent<A>();\n"
        "  if (auto* p = e.getComponent<A>()) p->x = 1;\n"
        "  auto q = e.getComponent<A>();\n}\n");
    CHECK(lspTest::count(*doc, RuleId::ComponentPointer) == 4u);
    std::vector<sfs::codegen::TextEdit> all;
    for (const auto& d : doc->diagnostics)
      all.insert(all.end(), d.fix.begin(), d.fix.end());
    const std::string text = lspTest::applyEdits(doc->text, all);
    CHECK(text ==
          "void f(sfs::Entity e)\n{\n  e.getComponent<A>()->x = 1;\n"
          "  use(registry->getComponent<A>(e));\n"
          "  auto* b = e.getComponent<B>();\n  b->y = 2;\n  take(*b);\n"
          "  push(b);\n  b->list[0] = 1;\n  C c = *e.getComponent<C>();\n"
          "  e.getComponent<A>()->x = 1;\n  A* a = e.getComponent<A>();\n"
          "  if (auto* p = e.getComponent<A>()) p->x = 1;\n"
          "  auto q = e.getComponent<A>();\n}\n");
    // The lookup now passed to use(), which the lint can't see: silent.
    CHECK(lspTest::count(*lspTest::analysed(text), RuleId::ComponentPointer) ==
          0u);
    // A lambda copying the reference would copy the component: no fix.
    const auto copied = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n  const A& a = e.getComponent<A>();\n"
        "  later([a] { use(a); });\n  later([=] { use(a.x); });\n}\n");
    CHECK(lspTest::count(*copied, RuleId::ComponentPointer) == 1u);
    CHECK(lspTest::first(*copied, RuleId::ComponentPointer) &&
          lspTest::first(*copied, RuleId::ComponentPointer)->fix.empty());
  }

  TEST("the entity API fixes should refuse what they can't rewrite exactly")
  {
    // A by-value handle passed on or given a component can't become a row's
    // const entity; one only read can.
    const auto byValue = lspTest::analysed(
        "void f()\n{\n  for (sfs::Entity e : registry->query<A>())\n  {\n"
        "    e.getComponent<A>().x = 1;\n    dress(e);\n  }\n"
        "  for (sfs::Entity e : registry->query<A>())\n  {\n"
        "    e.getComponent<A>().x = 1;\n    e.addTag<Seen>();\n  }\n"
        "  for (sfs::Entity e : registry->query<A>())\n"
        "    e.getComponent<A>().x = e.isValid();\n}\n");
    CHECK(lspTest::count(*byValue, RuleId::RowLookup) == 3u);
    std::size_t withFix = 0;
    for (const auto& d : byValue->diagnostics)
      if (d.rule == RuleId::RowLookup && !d.fix.empty())
        ++withFix;
    CHECK(withFix == 1u);

    // An early-out whose test isn't first (or after the handle's validity)
    // keeps its order: reported, not fixed.
    const auto order = lspTest::analysed(
        "void f(sfs::Entity e, bool known)\n{\n  if (known || "
        "!e.hasComponent<A>())\n    return;\n  e.getComponent<A>().x = "
        "1;\n}\n");
    CHECK(lspTest::count(*order, RuleId::HasThenGet) == 1u);
    CHECK(lspTest::first(*order, RuleId::HasThenGet) &&
          lspTest::first(*order, RuleId::HasThenGet)->fix.empty());

    // A keyword can't be a name made from a type.
    const auto keyword = lspTest::analysed(
        "void f(sfs::Entity e)\n{\n  if (e.hasComponent<Static>())\n"
        "    e.getComponent<Static>().x = 1;\n}\n");
    CHECK(lspTest::first(*keyword, RuleId::HasThenGet) &&
          lspTest::first(*keyword, RuleId::HasThenGet)->fix.empty());

    // A set-up call naming a local of the constructor stays.
    const auto local = lspTest::analysed(
        "class S : public sfs::System\n{\n  S()\n  {\n"
        "    auto added = [this](const sfs::Entity& e) { use(e); };\n"
        "    observe<A>(added);\n  }\n};\n",
        "/t/S.h");
    CHECK(lspTest::count(*local, RuleId::SetupInConstructor) == 1u);
    CHECK(lspTest::first(*local, RuleId::SetupInConstructor) &&
          lspTest::first(*local, RuleId::SetupInConstructor)->fix.empty());
  }

  TEST("component-pointer should report lookups passed on only where the "
       "callee takes T&, and fix whole assignments")
  {
    const auto doc = lspTest::analysed(
        "namespace Rigs\n{\nbool free(const Booking& booking);\n"
        "bool free(const sfs::Entity& machine)\n{\n"
        "  return free(e.getComponent<Booking>());\n}\n}\n"
        "void dress(Tree& tree, int level);\nvoid pick(Tree* tree);\n"
        "void spill(Grain& grain);\nvoid spill(void* anything);\n"
        "template <typename T> void show(const T& value);\n"
        "void f(sfs::Entity e)\n{\n"
        "  dress(e.getComponent<Tree>(), 1);\n"
        "  pick(e.getComponent<Tree>());\n"
        "  spill(e.getComponent<Grain>());\n"
        "  show(e.getComponent<Tree>());\n"
        "  unknown(e.getComponent<Tree>());\n"
        "  e.getComponent<A>() = A{};\n"
        "  if (!sfs::expect(e.getComponent<A>(), e)) return;\n}\n");
    // Warned: Rigs::free (beside free(Entity)), dress (Tree& at that place)
    // and spill (a void* overload beside Grain&). Silent: pick takes Tree*,
    // show is generic, unknown() can't be seen, sfs::expect checks it.
    CHECK(lspTest::count(*doc, RuleId::ComponentPointer) == 4u);
    std::vector<std::string> passed;
    std::vector<sfs::codegen::TextEdit> all;
    for (const auto& d : doc->diagnostics)
      if (d.rule == RuleId::ComponentPointer)
      {
        if (d.fix.empty())
          passed.push_back(d.message);
        all.insert(all.end(), d.fix.begin(), d.fix.end());
      }
    CHECK(passed.size() == 3u);
    CHECK(passed.size() == 3u &&
          passed[0] == "This passes getComponent<Booking>() to free(), which "
                       "takes Booking& here, but the lookup now gives a "
                       "pointer. Check it for null, then pass what it points "
                       "to.");
    CHECK(passed.size() == 3u &&
          passed[1].find("to dress(), which takes Tree& here") !=
              std::string::npos);
    CHECK(passed.size() == 3u &&
          passed[2].find("beside an overload taking a pointer") !=
              std::string::npos);
    CHECK(lspTest::applyEdits(doc->text, all)
              .find("  *e.getComponent<A>() = A{};\n") != std::string::npos);
  }

  TEST("component-pointer should find a passed-on lookup's callee in an "
       "included header")
  {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "sfsLspCallee";
    std::filesystem::remove_all(root);
    lspTest::writeFile(
        root / "utils/Trees.h",
        "#pragma once\nnamespace Trees\n{\n"
        "void dress(Tree& tree);\nvoid count(const Tree* tree);\n}\n");
    const std::string source =
        "#include \"../utils/Trees.h\"\nvoid f(sfs::Entity e)\n{\n"
        "  Trees::dress(e.getComponent<Tree>());\n"
        "  Trees::count(e.getComponent<Tree>());\n}\n";
    lspTest::writeFile(root / "systems/TreeSystem.cpp", source);
    const auto doc = lspTest::analysed(source, root / "systems/TreeSystem.cpp");
    CHECK(lspTest::count(*doc, RuleId::ComponentPointer) == 1u);
    CHECK(lspTest::first(*doc, RuleId::ComponentPointer) &&
          lspTest::first(*doc, RuleId::ComponentPointer)->range.startRow == 3u);
    std::filesystem::remove_all(root);
  }

  TEST("own-entity should see a component reached through a pointer")
  {
    CHECK(
        lspTest::count(*lspTest::analysed(
                           "void f(sfs::Entity e)\n{\n  if (auto* p = "
                           "e.getComponent<P>())\n    p->owner = e;\n  P* q = "
                           "e.getComponent<P>();\n  q->self = e;\n"
                           "  e.getComponent<P>()->me = e;\n}\n"),
                       RuleId::OwnEntity) == 3u);
  }

  TEST("--fix should apply the rules in order across a tree, then change "
       "nothing")
  {
    Project project;
    const std::filesystem::path header =
        project.root / "scripts/systems/MoverSystem.h";
    const std::filesystem::path source =
        project.root / "scripts/systems/MoverSystem.cpp";
    lspTest::writeFile(header,
                       "#pragma once\nclass MoverSystem : public sfs::System\n"
                       "{\npublic:\n  MoverSystem();\n\nprotected:\n"
                       "  void update(double deltaTime) override;\n};\n");
    lspTest::writeFile(
        source,
        "#include \"MoverSystem.h\"\n\nMoverSystem::MoverSystem()\n{\n"
        "  registerComponent<Mover>();\n}\n\n"
        "void MoverSystem::update(double)\n{\n"
        "  for (const sfs::Entity& e : getEntities())\n  {\n"
        "    Mover& mover = e.getComponent<Mover>();\n    mover.t += 1;\n"
        "    if (e.hasComponent<Trail>())\n"
        "      e.getComponent<Trail>().n += 1;\n    if (target.isAlive())\n"
        "      mover.target = target;\n  }\n}\n");
    const std::filesystem::path output = project.root / "fix.txt";
    const std::string command =
        "\"" SFS_LSP_PATH
        "\" --fix setup-in-constructor,is-alive,has-then-get,row-lookup,"
        "component-pointer --root \"" +
        project.root.string() + "\" \"" + (project.root / "scripts").string() +
        "\" > \"" + output.string() + "\"";
    CHECK(std::system(command.c_str()) == 0);
    const auto read = [](const std::filesystem::path& path)
    {
      std::ifstream in(path);
      std::stringstream text;
      text << in.rdbuf();
      return text.str();
    };
    CHECK(read(header) == "#pragma once\nclass MoverSystem : public "
                          "sfs::System\n{\nprotected:\n"
                          "  void create() override;\n"
                          "  void update(double deltaTime) override;\n};\n");
    CHECK(read(source) ==
          "#include \"MoverSystem.h\"\n\nvoid MoverSystem::create()\n{\n"
          "  registerComponent<Mover>();\n}\n\n"
          "void MoverSystem::update(double)\n{\n"
          "  for (auto [e, mover] : getEntities<Mover>())\n  {\n"
          "    mover.t += 1;\n"
          "    if (Trail* trail = e.getComponent<Trail>())\n"
          "      trail->n += 1;\n    if (target.isAlive())\n"
          "      mover.target = target;\n  }\n}\n");
    const std::string scripts = (project.root / "scripts/systems/").string();
    CHECK(read(output) ==
          scripts +
              "MoverSystem.cpp:5:3: fixed [setup-in-constructor] Move "
              "the set-up to create()\n" +
              scripts +
              "MoverSystem.cpp:14:9: fixed [has-then-get] Look "
              "Trail up once\n" +
              scripts +
              "MoverSystem.cpp:12:20: fixed [row-lookup] Unpack the "
              "row\n");
    // A second run finds nothing to change.
    const std::string before = read(source);
    CHECK(std::system(command.c_str()) == 0);
    CHECK(read(source) == before);
    CHECK(read(output).empty());
  }

  TEST("--check should report every rule over a project, from any folder")
  {
    // A whole rule gone quiet shows here: each must report at least once.
    Project project;
    for (const auto& [path, text] : everyRuleFixtures())
      lspTest::writeFile(project.root / "scripts" / path, text);
    const std::filesystem::path output = project.root / "every.txt";
    const std::string run = "cd \"" +
                            std::filesystem::temp_directory_path().string() +
                            "\" && \"" SFS_LSP_PATH "\" --check --root \"" +
                            project.root.string() + "\" ";
    // The path is the root's, not the working folder's. The fixtures have
    // errors, so it exits with 1.
    CHECK(std::system(
              (run + "scripts > \"" + output.string() + "\"").c_str()) != 0);
    std::ifstream in(output);
    std::stringstream printed;
    printed << in.rdbuf();
    for (const sfs::codegen::RuleInfo& rule : sfs::codegen::rules())
    {
      if (rule.reserved)
        continue;
      const bool reported = printed.str().find("[" + std::string(rule.name) +
                                               "]") != std::string::npos;
      if (!reported)
        std::printf(
            "      --check didn't report %s\n", std::string(rule.name).c_str());
      CHECK(reported);
    }
    // Paths with no C++ files under them are an error, not a clean run.
    const std::filesystem::path errors = project.root / "errors.txt";
    CHECK(std::system(
              (run + "nowhere 2> \"" + errors.string() + "\"").c_str()) != 0);
    std::ifstream errorsIn(errors);
    std::stringstream complaint;
    complaint << errorsIn.rdbuf();
    CHECK(complaint.str().find("no C++ files") != std::string::npos);
  }

  TEST("every rule's messages and summary should be plain sentences")
  {
    // Fixtures that make every rule report, codegen's included.
    Project project;
    std::vector<std::unique_ptr<Document>> docs;
    for (const auto& [path, text] : everyRuleFixtures())
      docs.push_back(lspTest::analysed(
          text, project.root / "scripts" / path, &project.index));
    std::vector<std::size_t> seen(static_cast<std::size_t>(RuleId::Count), 0);
    const auto sentence = [](std::string_view text)
    {
      return !text.empty() &&
             ((text.front() >= 'A' && text.front() <= 'Z') ||
              text.front() == '`') &&
             text.back() == '.';
    };
    for (const auto& doc : docs)
      for (const auto& d : doc->diagnostics)
      {
        ++seen[static_cast<std::size_t>(d.rule)];
        if (!sentence(d.message))
          std::printf("      not a sentence [%s]: %s\n",
                      std::string(sfs::codegen::ruleInfo(d.rule).name).c_str(),
                      d.message.c_str());
        CHECK(sentence(d.message));
      }
    for (const sfs::codegen::RuleInfo& rule : sfs::codegen::rules())
    {
      // The summary reads the same way (a code name may start it).
      CHECK(sentence(rule.summary));
      if (rule.reserved)
        continue;
      if (seen[static_cast<std::size_t>(rule.id)] == 0)
        std::printf(
            "      no fixture reports %s\n", std::string(rule.name).c_str());
      CHECK(seen[static_cast<std::size_t>(rule.id)] > 0);
    }
  }

  return testing::report("lspLintTests");
}
