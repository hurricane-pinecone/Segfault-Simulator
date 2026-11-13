#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

/**
 * One key or scalar value of a JSON text, found by scanJsonTokens(): where it
 * is in the text and which value it is, so a tool can change it by editing
 * just those bytes and leave the rest of the file exactly as it was.
 */
struct JsonToken
{
  enum class Kind
  {
    Key,     // an object member's name
    String,  // a string value
    Number,  // a number value
    Literal, // true, false or null
  };

  /** What the token is. */
  Kind kind = Kind::String;
  /**
   * The RFC 6901 JSON pointer of the value ("/root/$overrides/1~115"); for a
   * key, of the member it names.
   */
  std::string pointer;
  /** Where it is in the text: [begin, end), a string's quotes included. */
  std::size_t begin = 0;
  std::size_t end = 0;
  /** A key's or string's decoded text; a number's or literal's spelling. */
  std::string text;
};

/**
 * Every key and scalar value of a JSON text, in the order they appear.
 *
 * @param text the JSON text (UTF-8)
 * @return nothing if it isn't valid JSON
 */
std::optional<std::vector<JsonToken>> scanJsonTokens(std::string_view text);

/**
 * One JSON pointer segment, escaped as RFC 6901 asks ("~" as "~0", "/" as
 * "~1"), to be appended to a pointer after a "/".
 *
 * @param segment a member name or an array index
 * @return the escaped segment
 */
std::string jsonPointerSegment(std::string_view segment);

/** A replacement of a byte range of a text. */
struct TextEdit
{
  /** The range replaced: [begin, end). */
  std::size_t begin = 0;
  std::size_t end = 0;
  /** What goes there instead. */
  std::string text;
};

/**
 * A text with byte ranges replaced and every other byte kept as it was.
 *
 * @param text  the original text
 * @param edits the replacements, in any order; ranges must not overlap
 * @return the edited text, or nothing if a range is outside the text or two
 *         overlap
 */
std::optional<std::string> applyTextEdits(std::string_view text,
                                          std::vector<TextEdit> edits);

} // namespace sfs
