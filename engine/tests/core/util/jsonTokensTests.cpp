#include "../../testHarness.h"

#include <engine/core/util/jsonTokens.h>

#include <string>
#include <string_view>
#include <vector>

using namespace sfs;

namespace
{
const JsonToken* find(const std::vector<JsonToken>& tokens,
                      std::string_view pointer,
                      JsonToken::Kind kind)
{
  for (const JsonToken& token : tokens)
    if (token.pointer == pointer && token.kind == kind)
      return &token;
  return nullptr;
}
} // namespace

int main()
{
  TEST("every key and scalar value should have its pointer and byte range")
  {
    const std::string text = R"({
  "formatVersion": 1,
  "root": {
    "children": [
      {"$id": 3, "$overrides": {"1/15": {"A": {"on": true, "x": -1.5e3}}}},
      {"$removed": ["2", "7"], "none": null}
    ]
  }
})";
    const auto tokens = scanJsonTokens(text);
    CHECK(tokens.has_value());
    if (!tokens)
      return testing::report("jsonTokensTests");

    const JsonToken* version =
        find(*tokens, "/formatVersion", JsonToken::Kind::Number);
    CHECK(version != nullptr);
    if (version)
    {
      CHECK(version->text == "1");
      CHECK(text.substr(version->begin, version->end - version->begin) == "1");
    }
    // A key with a slash: escaped in the pointer, decoded in the text.
    const JsonToken* key = find(
        *tokens, "/root/children/0/$overrides/1~115", JsonToken::Kind::Key);
    CHECK(key != nullptr);
    if (key)
    {
      CHECK(key->text == "1/15");
      CHECK(text.substr(key->begin, key->end - key->begin) == "\"1/15\"");
    }
    const JsonToken* removed =
        find(*tokens, "/root/children/1/$removed/1", JsonToken::Kind::String);
    CHECK(removed != nullptr && removed->text == "7");
    CHECK(find(*tokens,
               "/root/children/0/$overrides/1~115/A/on",
               JsonToken::Kind::Literal) != nullptr);
    const JsonToken* number = find(*tokens,
                                   "/root/children/0/$overrides/1~115/A/x",
                                   JsonToken::Kind::Number);
    CHECK(number != nullptr && number->text == "-1.5e3");
    CHECK(find(*tokens, "/root/children/1/none", JsonToken::Kind::Literal) !=
          nullptr);
    // Containers have no tokens of their own, only their keys.
    CHECK(find(*tokens, "/root", JsonToken::Kind::Key) != nullptr);
    CHECK(find(*tokens, "/root", JsonToken::Kind::String) == nullptr);
  }

  TEST("escapes and unicode should decode, their ranges cover the spelling")
  {
    const std::string text = R"({"a\"b": "x\\y\/z", "~k": "é😀\n"})";
    const auto tokens = scanJsonTokens(text);
    CHECK(tokens.has_value());
    if (!tokens)
      return testing::report("jsonTokensTests");
    const JsonToken* quoted = find(*tokens, "/a\"b", JsonToken::Kind::Key);
    CHECK(quoted != nullptr);
    if (quoted)
      CHECK(text.substr(quoted->begin, quoted->end - quoted->begin) ==
            R"("a\"b")");
    const JsonToken* slashes = find(*tokens, "/a\"b", JsonToken::Kind::String);
    CHECK(slashes != nullptr && slashes->text == "x\\y/z");
    const JsonToken* unicode = find(*tokens, "/~0k", JsonToken::Kind::String);
    CHECK(unicode != nullptr);
    if (unicode)
      CHECK(unicode->text == "\xC3\xA9\xF0\x9F\x98\x80\n");
  }

  TEST("text that isn't JSON should give nothing")
  {
    CHECK(!scanJsonTokens(R"({"a": 1,})"));
    CHECK(!scanJsonTokens(R"({"a" 1})"));
    CHECK(!scanJsonTokens(R"(["a")"));
    CHECK(!scanJsonTokens(R"({"a": 01x})"));
    CHECK(!scanJsonTokens(R"({"a": tru})"));
    CHECK(!scanJsonTokens(R"({"a": "b"} extra)"));
    CHECK(!scanJsonTokens(std::string(2000, '[') + std::string(2000, ']')));
    CHECK(scanJsonTokens(" [ ] ").has_value());
  }

  TEST("replacing tokens should leave every other byte unchanged")
  {
    const std::string text = "{\n  \"formatVersion\": 1,\n"
                             "  \"$overrides\": {\"15\": {\"A\": 2},  "
                             "\"3\": {}},\n  \"$parent\": \"7\"\n}\n";
    const auto tokens = scanJsonTokens(text);
    CHECK(tokens.has_value());
    if (!tokens)
      return testing::report("jsonTokensTests");
    const JsonToken* version =
        find(*tokens, "/formatVersion", JsonToken::Kind::Number);
    const JsonToken* key =
        find(*tokens, "/$overrides/15", JsonToken::Kind::Key);
    const JsonToken* parent =
        find(*tokens, "/$parent", JsonToken::Kind::String);
    CHECK(version && key && parent);
    if (!version || !key || !parent)
      return testing::report("jsonTokensTests");
    // Given out of order, and one longer than what it replaces.
    const auto edited =
        applyTextEdits(text,
                       {{parent->begin, parent->end, "\"2147483700\""},
                        {version->begin, version->end, "2"},
                        {key->begin, key->end, "\"2318043377\""}});
    CHECK(edited.has_value());
    if (edited)
      CHECK(*edited == "{\n  \"formatVersion\": 2,\n"
                       "  \"$overrides\": {\"2318043377\": {\"A\": 2},  "
                       "\"3\": {}},\n  \"$parent\": \"2147483700\"\n}\n");
    CHECK(!applyTextEdits(text, {{2, 5, "a"}, {4, 6, "b"}}));
    CHECK(!applyTextEdits(text, {{2, text.size() + 1, "a"}}));
    CHECK(applyTextEdits(text, {}) == std::optional<std::string>(text));
  }

  TEST("a pointer segment should escape ~ and /")
  {
    CHECK(jsonPointerSegment("1/15") == "1~115");
    CHECK(jsonPointerSegment("a~b") == "a~0b");
    CHECK(jsonPointerSegment("") == "");
  }

  return testing::report("jsonTokensTests");
}
