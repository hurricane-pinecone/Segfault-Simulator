#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The [[sfs::...]] attributes: what codegen accepts, and what sfs-lsp completes
// and documents. The one list both tools and the docs read.
namespace sfs::codegen
{

/**
 * Which attribute an entry is: the parser switches on it, so a new entry
 * without a parser case is a -Wswitch warning.
 */
enum class AttributeId : uint8_t
{
  Range,
  Color,
  Hide,
  Show,
  Script,
  Texture,
  Materials,
  Material,
  EmissionLight,
  ShowIf,
  Part,
  Component,
};

/** Where an attribute is written. */
enum class AttributeTarget : uint8_t
{
  Field,
  Type,
};

/** What an attribute's parentheses hold. */
enum class AttributeArgs : uint8_t
{
  None,      // [[sfs::hide]]
  MinMax,    // (min, max): two expressions
  BoolField, // (field) or (!field): a bool field of the same type
  String,    // ("text"): one non-empty string literal
};

/**
 * One [[sfs::...]] attribute: what codegen accepts, and what sfs-lsp completes
 * and documents.
 */
struct AttributeInfo
{
  /** The parser's case for it. */
  AttributeId id;
  /** As written after `sfs::`, e.g. "range". */
  std::string_view name;
  /** On a field or on a type. */
  AttributeTarget target;
  /** What its parentheses take. */
  AttributeArgs args;
  /**
   * The field kinds (FieldModel::kind names) it applies to; none listed: any.
   */
  std::array<std::string_view, 3> kinds;
  /** Its arguments as documented, e.g. "(min, max)"; empty with none. */
  std::string_view arguments;
  /** The arguments as an LSP snippet, e.g. "(${1:min}, ${2:max})". */
  std::string_view snippet;
  /**
   * One sentence: the docs table's Effect, the completion docs and the hover.
   */
  std::string_view summary;
  /**
   * A short real use of it, lines separated by '\n': the hover's and
   * completion's code block, and the docs table's Example.
   */
  std::string_view example;
};

/** Every attribute, in the order the docs list them. */
std::span<const AttributeInfo> attributes();

/** The attribute called `name`, or null. */
const AttributeInfo* findAttribute(std::string_view name);

/**
 * The attribute a misspelt `name` most likely meant (case-insensitive, at most
 * 2 edits), or null.
 */
const AttributeInfo* closestAttribute(std::string_view name);

/**
 * Whether `info` applies to a field of `kind` (a FieldModel::kind, e.g.
 * "Vec3").
 */
bool appliesTo(const AttributeInfo& info, std::string_view kind);

/**
 * The C++ types of the fields `info` applies to, as a phrase.
 *
 * @param info the attribute
 * @param quote written around each type, e.g. "`" for Markdown
 * @return e.g. "glm::vec3 and glm::vec4"; empty when it applies to any field
 */
std::string attributeFieldTypes(const AttributeInfo& info,
                                std::string_view quote = {});

/** The docs' attribute table (docs/core/reflection/index.md), as Markdown. */
std::string attributeDocsTable();

/**
 * The edit distance between two names, ignoring case: how many single
 * characters must be inserted, removed or changed to turn one into the other.
 * The did-you-mean suggestions of codegen and sfs-lsp use it.
 *
 * @param a one name
 * @param b the other
 * @return the number of edits
 */
std::size_t editDistance(std::string_view a, std::string_view b);

} // namespace sfs::codegen
