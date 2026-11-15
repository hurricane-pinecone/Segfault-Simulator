#pragma once

#include "sfs/codegen/rules.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What sfs-codegen learns from a header: the script types it declares and their
// editable fields. Produced by the parser, consumed by the generator.
namespace sfs::codegen
{

enum class TypeKind
{
  Component,
  System,
  GameObject,
  MeshSource,
  ScriptableObject, // derives from sfs::ScriptableObject
  // Derives from another reflected type (the engine's or the module's own,
  // e.g. `struct TractorSpec : sfs::VehicleSpec`): its kind is its base's,
  // decided when the generated code compiles; for now a scriptable object.
  Derived,
};

/**
 * A span of a source file: tree-sitter points (0-based rows, byte columns), end
 * exclusive.
 */
struct SourceRange
{
  unsigned startRow = 0;
  unsigned startColumn = 0;
  unsigned endRow = 0;
  unsigned endColumn = 0;
};

/** One edit of a fix: replace `range` with `text`. */
struct TextEdit
{
  /** What it replaces; empty to insert at its start. */
  SourceRange range;
  /** The new text. */
  std::string text;
  /**
   * The file it edits; empty for the diagnostic's own file. Set when a fix
   * spans files (a class's header and its .cpp).
   */
  std::filesystem::path file;
};

struct FieldModel
{
  std::string name;
  std::string type;       // as written, whitespace-normalised, e.g. "glm::vec3"
  std::string kind;       // reflect::FieldKind enumerator, e.g. "Vec3"
  bool isPublic = true;   // false: private/protected, exposed by [[sfs::show]]
  bool color = false;     // [[sfs::color]]
  bool script = false;    // [[sfs::script]]
  bool texture = false;   // [[sfs::texture]]
  bool materials = false; // [[sfs::materials]]
  bool material = false;  // [[sfs::material]]
  bool emissionLight = false; // [[sfs::emissionLight]]
  std::string showIf;         // [[sfs::showIf(field)]]: a bool field's name
                              // ("!name": while it's off)
  std::string assetType; // AssetRef: the scriptable object type, e.g. "ItemDef"
  std::string part; // [[sfs::part("name")]]: an Entity field's node name/glob
  std::optional<std::pair<std::string, std::string>> range; // [[sfs::range]]
  /** Where its name is written in the header. */
  SourceRange nameRange;
};

struct TypeModel
{
  std::string name;          // unqualified, e.g. "Player"
  std::string qualifiedName; // with namespaces, e.g. "game::Player"
  TypeKind kind = TypeKind::Component;
  bool uiLayer = false; // a GameObject that's an sfs::ui::Layer
  std::string base;     // Derived: the base's name, unqualified ("VehicleSpec")
  std::vector<FieldModel> fields;
  std::filesystem::path header;
  unsigned line = 0; // 1-based, where it's declared
  /** Where its name is written in the header. */
  SourceRange nameRange;
};

/**
 * A problem codegen or an sfs-lsp lint found. Its severity, source and tags
 * are its rule's (ruleInfo(rule)).
 */
struct Diagnostic
{
  /** The rule it belongs to: what sfs-ignore names, and its severity. */
  RuleId rule = RuleId::NoScriptType;
  /** The file it's in. */
  std::filesystem::path file;
  /** The code it's about; it belongs to the line the range starts on. */
  SourceRange range;
  /** What's wrong, in a sentence. */
  std::string message;
  /** The title of its fix, e.g. "Change to [[sfs::hide]]"; empty: no fix. */
  std::string fixTitle;
  /** The fix's edits, applied together. */
  std::vector<TextEdit> fix;
};

/** The severity of `diagnostic`: its rule's. */
inline Severity severityOf(const Diagnostic& diagnostic)
{
  return ruleInfo(diagnostic.rule).severity;
}

/**
 * A diagnostic as the build prints it: `file:line: severity: message [rule]`,
 * or with `column`, `file:line:column: ...` (1-based, in bytes).
 */
std::string formatDiagnostic(const Diagnostic& diagnostic, bool column);

/** The sfs-ignore comments of one file. */
struct Suppressions
{
  /** Rules ignored in the whole file. */
  RuleSet file;
  /** Rules ignored on single lines: 0-based row and rules, sorted by row. */
  std::vector<std::pair<unsigned, RuleSet>> lines;
};

struct ParseResult
{
  std::optional<TypeModel> type; // the header's script type, if found
  /** [[sfs::component]] types besides the file's, in declaration order. */
  std::vector<TypeModel> components;
  std::vector<Diagnostic> diagnostics;
  /** The header's sfs-ignore comments, already applied to `diagnostics`. */
  Suppressions suppressions;
};

// A struct in a UI header (*.ui.h): a view (it has body()) or something one
// shows (a list's item). The editor's UI Preview shows each view filled with
// placeholders, field by field.
struct UiTypeModel
{
  std::string name;                // unqualified, e.g. "ClockPanel"
  std::string qualifiedName;       // with namespaces, e.g. "farm::ClockPanel"
  bool view = false;               // it has a body()
  std::vector<std::string> fields; // public data members, in order
  std::filesystem::path header;
};

struct UiParseResult
{
  std::vector<UiTypeModel> types; // in the order declared
  std::vector<Diagnostic> diagnostics;
};

} // namespace sfs::codegen
