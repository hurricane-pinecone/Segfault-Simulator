#include "../testHarness.h"
#include "lspFeatures.h"
#include "lspTestSupport.h"

#include <sfs/codegen/attributes.h>
#include <sfs/codegen/rules.h>

#include <set>

using namespace sfs::lsp;
using nlohmann::json;

namespace
{

ClientSettings snippets()
{
  ClientSettings settings;
  settings.encoding = PositionEncoding::Utf8;
  settings.snippets = true;
  return settings;
}

// The text with `|` marking the cursor: the document without it, and where it
// was.
struct Marked
{
  std::unique_ptr<Document> document;
  Position cursor;
};

Marked marked(std::string text, std::filesystem::path path = "/t/Thing.h")
{
  const std::size_t at = text.find('|');
  text.erase(at, 1);
  const std::vector<uint32_t> starts = countLines(text);
  uint32_t line = 0;
  while (line + 1 < starts.size() && starts[line + 1] <= at)
    ++line;
  return {lspTest::analysed(std::move(text), std::move(path)),
          {line, static_cast<uint32_t>(at - starts[line])}};
}

std::vector<std::string> labels(const json& list)
{
  std::vector<std::string> out;
  for (const json& item : list["items"])
    out.push_back(item["label"]);
  return out;
}

bool has(const std::vector<std::string>& list, std::string_view label)
{
  return std::find(list.begin(), list.end(), label) != list.end();
}

std::vector<std::string> complete(std::string text,
                                  bool invoked = false,
                                  ClientSettings settings = snippets())
{
  Marked at = marked(std::move(text));
  return labels(completion(*at.document, at.cursor, invoked, settings));
}

} // namespace

int main()
{
  TEST("[[sfs:: in a class body should list the field attributes")
  {
    const auto items =
        complete("struct Thing\n{\n  [[sfs::|]] float a = 0;\n};\n");
    CHECK(has(items, "range"));
    CHECK(has(items, "hide"));
    CHECK(has(items, "showIf"));
    CHECK(!has(items, "component")); // a type's
    CHECK(!has(items, "color"));     // not for a float
    CHECK(!has(items, "show"));      // public already
    CHECK(!has(items, "part"));
  }

  TEST("[[ should list the same items, labelled sfs::name")
  {
    const auto items =
        complete("struct Thing\n{\n  [[|]] glm::vec3 tint;\n};\n");
    CHECK(has(items, "sfs::color"));
    CHECK(!has(items, "sfs::range"));
  }

  TEST("a comma inside an attribute's arguments should offer nothing")
  {
    CHECK(complete("struct Thing\n{\n  [[sfs::range(0, |]] float a = 0;\n};\n")
              .empty());
    CHECK(has(complete("struct Thing\n{\n  [[sfs::range(0, 1), |]] float a = "
                       "0;\n};\n"),
              "sfs::hide"));
  }

  TEST("attributes already on the declaration should be left out")
  {
    const auto items =
        complete("struct Thing\n{\n  [[sfs::hide, sfs::|]] float a = 0;\n};\n");
    CHECK(!has(items, "hide"));
    CHECK(has(items, "range"));
  }

  TEST("a class head should list the type attributes; a private field show")
  {
    const auto head = complete("struct [[sfs::|]] Thing\n{\n  int a;\n};\n");
    CHECK(head == std::vector<std::string>{"component"});
    const auto hidden =
        complete("class Thing\n{\n  [[sfs::|]] int a = 0;\n};\n");
    CHECK(has(hidden, "show"));
    CHECK(!has(hidden, "hide"));
  }

  TEST("an untyped field should list every field attribute")
  {
    const auto items = complete("struct Thing\n{\n  [[sfs::|\n};\n");
    std::size_t fieldAttributes = 0;
    for (const auto& info : sfs::codegen::attributes())
      if (info.target == sfs::codegen::AttributeTarget::Field &&
          info.id != sfs::codegen::AttributeId::Show)
        ++fieldAttributes;
    CHECK(items.size() == fieldAttributes);
  }

  TEST("showIf( should list the class's bool fields")
  {
    const auto items =
        complete("struct Thing\n{\n  bool on = false;\n  bool off, other;\n  "
                 "int count;\n  [[sfs::showIf(!|)]] float a;\n};\n");
    CHECK(items == (std::vector<std::string>{"on", "off", "other"}));
  }

  TEST("snippets should be used only when the client takes them")
  {
    Marked at = marked("struct Thing\n{\n  [[sfs::ra|]] float a = 0;\n};\n");
    const json list = completion(*at.document, at.cursor, false, snippets());
    ClientSettings plain = snippets();
    plain.snippets = false;
    const json plainList = completion(*at.document, at.cursor, false, plain);
    for (const json& item : list["items"])
      if (item["label"] == "range")
      {
        CHECK(item["textEdit"]["newText"] == "range(${1:min}, ${2:max})");
        CHECK(item["textEdit"]["range"]["start"]["character"] == 9);
        CHECK(item["documentation"]["value"].get<std::string>().find(
                  "```cpp\n[[sfs::range(10, 120)]] float fieldOfView") !=
              std::string::npos);
      }
    for (const json& item : plainList["items"])
      if (item["label"] == "range")
      {
        CHECK(item["textEdit"]["newText"] == "range");
        CHECK(item["insertTextFormat"] == 1);
      }
  }

  TEST(
      "nothing should be offered in other comments, strings or outside classes")
  {
    CHECK(complete("struct Thing\n{\n  // [[sfs::|\n  int a;\n};\n").empty());
    CHECK(complete("const char* s = \"[[sfs::|\";\n").empty());
    CHECK(complete("[[sfs::|]] int free;\n").empty());
    CHECK(complete("int x = 1;|\n").empty());
  }

  TEST("sfs-ignore lists should offer the warning rules, those reported on the "
       "line first")
  {
    Marked at = marked("void f()\n{\n  // sfs-ignore-next-line(|)\n  query<A, "
                       "sfs::Any<B>>();\n}\n",
                       "/t/f.cpp");
    const json list = completion(*at.document, at.cursor, false, snippets());
    const auto items = labels(list);
    CHECK(has(items, "single-any"));
    CHECK(has(items, "own-entity"));
    CHECK(!has(items, "duplicate-type")); // an error
    CHECK(!has(items, "bad-ignore"));
    CHECK(!has(items, "renamed-engine-type")); // reserved: not reported yet
    std::string firstSort;
    std::string singleSort;
    for (const json& item : list["items"])
    {
      if (firstSort.empty() || item["sortText"].get<std::string>() < firstSort)
        firstSort = item["sortText"];
      if (item["label"] == "single-any")
        singleSort = item["sortText"];
    }
    CHECK(singleSort == firstSort);

    const auto afterComma =
        complete("int a; // sfs-ignore(own-entity, sing|)\n", false);
    CHECK(has(afterComma, "single-any"));
    CHECK(!has(afterComma, "own-entity")); // named already
    CHECK(has(complete("// sfs-ignore-file(|\n"), "no-script-type"));
  }

  TEST("a comment beginning sfs- should list the directive names when asked")
  {
    const auto items = complete("int a;\n// sfs-|\n", true);
    CHECK(items ==
          (std::vector<std::string>{
              "sfs-ignore", "sfs-ignore-next-line", "sfs-ignore-file"}));
    CHECK(complete("int a;\n// sfs-|\n", false).empty());
    CHECK(complete("int a;\n// other sfs-|\n", true).empty());
  }

  TEST("hover should document every attribute and every rule id in a directive")
  {
    const ClientSettings settings = snippets();
    for (const auto& info : sfs::codegen::attributes())
    {
      const std::string name(info.name);
      const bool type = info.target == sfs::codegen::AttributeTarget::Type;
      const std::string text =
          type ? "struct [[sfs::" + name + "]] Thing { int a; };\n"
               : "struct Thing { [[sfs::" + name + "]] int a; };\n";
      auto doc = lspTest::document(text, "/t/Thing.h");
      const Position over{0, static_cast<uint32_t>(text.find(name) + 1)};
      const json answer = hover(*doc, over, settings);
      CHECK(answer.is_object() &&
            answer["contents"]["value"].get<std::string>().find(
                std::string(info.summary)) != std::string::npos);
      CHECK(answer.is_object() &&
            answer["contents"]["value"].get<std::string>().find(
                "```cpp\n" + std::string(info.example) + "\n```") !=
                std::string::npos);
    }
    for (const auto& rule : sfs::codegen::rules())
    {
      if (rule.reserved)
        continue;
      const std::string text =
          "int a; // sfs-ignore(x, " + std::string(rule.name) + ")\n";
      auto doc = lspTest::document(text);
      const json answer = hover(
          *doc, {0, static_cast<uint32_t>(text.find(rule.name) + 2)}, settings);
      CHECK(answer.is_object() &&
            answer["contents"]["value"].get<std::string>().find(
                std::string(rule.summary)) != std::string::npos);
    }
    auto plain = lspTest::document("int value = 3; // nothing here\n");
    CHECK(hover(*plain, {0, 5}, settings).is_null());
    CHECK(hover(*plain, {0, 20}, settings).is_null());
  }

  return testing::report("lspCompletionTests");
}
