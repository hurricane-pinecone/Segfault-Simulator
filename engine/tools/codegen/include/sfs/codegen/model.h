#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// What sfs-codegen learns from a header: the script type it declares and that
// type's editable fields. Produced by the parser, consumed by the generator.
namespace sfs::codegen
{

enum class TypeKind
{
  Component,
  System,
  GameObject,
  MeshSource,
  ScriptableObject, // derives from sfs::ScriptableObject
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
  bool emissionLight = false; // [[sfs::emissionLight]]
  std::string assetType; // AssetRef: the scriptable object type, e.g. "ItemDef"
  std::optional<std::pair<std::string, std::string>> range; // [[sfs::range]]
};

struct TypeModel
{
  std::string name;          // unqualified, e.g. "Player"
  std::string qualifiedName; // with namespaces, e.g. "game::Player"
  TypeKind kind = TypeKind::Component;
  std::vector<FieldModel> fields;
  std::filesystem::path header;
};

struct Diagnostic
{
  enum class Severity
  {
    Warning,
    Error,
  };

  Severity severity = Severity::Warning;
  std::filesystem::path file;
  unsigned line = 0; // 1-based
  std::string message;
};

struct ParseResult
{
  std::optional<TypeModel> type; // the header's script type, if found
  std::vector<Diagnostic> diagnostics;
};

} // namespace sfs::codegen
