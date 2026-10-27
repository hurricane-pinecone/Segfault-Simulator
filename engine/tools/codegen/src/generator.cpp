#include "sfs/codegen/generator.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

namespace sfs::codegen
{

namespace
{

// The C++ type each field kind stands for, spelled so it resolves from the
// generated file (outside any namespace).
const std::map<std::string, std::string, std::less<>>& canonicalTypes()
{
  static const std::map<std::string, std::string, std::less<>> types = {
      {"Bool", "bool"},
      {"Int", "std::int32_t"},
      {"UInt", "std::uint32_t"},
      {"Float", "float"},
      {"String", "std::string"},
      {"Vec2", "glm::vec2"},
      {"Vec3", "glm::vec3"},
      {"Vec4", "glm::vec4"},
      {"Quat", "glm::quat"},
      {"Mesh", "sfs::MeshHandle"},
      {"Json", "nlohmann::json"},
  };
  return types;
}

// A C++ string literal for `text`.
std::string literal(std::string_view text)
{
  std::string out = "\"";
  for (const char c : text)
  {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  return out + '"';
}

// "game::Player" -> "game_Player", for generated identifiers.
std::string identifier(std::string_view qualified)
{
  std::string out;
  for (std::size_t i = 0; i < qualified.size(); ++i)
  {
    if (qualified.substr(i, 2) == "::")
    {
      out += '_';
      ++i;
      continue;
    }
    const auto c = static_cast<unsigned char>(qualified[i]);
    out += std::isalnum(c) ? static_cast<char>(c) : '_';
  }
  return out;
}

std::string includePath(const std::filesystem::path& header,
                        const std::filesystem::path& root)
{
  if (!root.empty())
  {
    const std::filesystem::path relative = header.lexically_relative(root);
    if (!relative.empty() && *relative.begin() != "..")
      return relative.generic_string();
  }
  return header.generic_string();
}

std::string accessTag(const TypeModel& type, const FieldModel& field)
{
  return "Access_" + identifier(type.qualifiedName) + "_" + field.name;
}

void emitAccessTags(std::ostringstream& out, const std::vector<TypeModel>& types)
{
  bool any = false;
  for (const TypeModel& type : types)
  {
    for (const FieldModel& field : type.fields)
    {
      if (field.isPublic)
        continue;
      if (!any)
        // The tags must share MemberAccess's namespace: the friend it defines
        // belongs to that namespace, and is found from the tag by ADL.
        out << "// Accessors for non-public [[sfs::show]] fields (see "
               "sfs::reflect::MemberAccess).\nnamespace sfs::reflect\n{\n";
      any = true;

      const std::string tag = accessTag(type, field);
      out << "struct " << tag << "\n{\n  using type = "
          << canonicalTypes().at(field.kind) << " " << type.qualifiedName
          << "::*;\n  friend type memberOf(" << tag << ");\n};\n"
          << "template struct MemberAccess<" << tag << ", &"
          << type.qualifiedName << "::" << field.name << ">;\n\n";
    }
  }
  if (any)
    out << "} // namespace sfs::reflect\n\n";
}

void emitField(std::ostringstream& out, const TypeModel& type, const FieldModel& field)
{
  const std::string& cppType = canonicalTypes().at(field.kind);

  out << "    {\n";
  if (field.isPublic)
    out << "      static_assert(std::is_same_v<std::remove_cvref_t<decltype("
           "std::declval<"
        << type.qualifiedName << "&>()." << field.name << ")>, " << cppType
        << ">,\n                    " << literal(type.name + "::" + field.name +
                                                " must be " + cppType)
        << ");\n";

  out << "      FieldInfo field;\n"
      << "      field.name = " << literal(field.name) << ";\n"
      << "      field.displayName = " << literal(displayName(field.name, false))
      << ";\n"
      << "      field.kind = FieldKind::" << field.kind << ";\n";
  if (field.color)
    out << "      field.attributes.color = true;\n";
  if (field.script)
    out << "      field.attributes.script = true;\n";
  if (field.range)
    out << "      field.attributes.hasRange = true;\n"
        << "      field.attributes.min = static_cast<float>(" << field.range->first
        << ");\n"
        << "      field.attributes.max = static_cast<float>(" << field.range->second
        << ");\n";

  out << "      field.address = [](void* object) -> void* {\n";
  if (field.isPublic)
    out << "        return &static_cast<" << type.qualifiedName << "*>(object)->"
        << field.name << ";\n";
  else
    out << "        return &(static_cast<" << type.qualifiedName
        << "*>(object)->*memberOf(" << accessTag(type, field) << "{}));\n";
  out << "      };\n"
      << "      type.fields.push_back(std::move(field));\n"
      << "    }\n";
}

void emitType(std::ostringstream& out, const TypeModel& type, const GeneratorOptions& options)
{
  const std::string name = literal(type.name);
  const std::string module = literal(options.module);

  out << "  { // " << type.qualifiedName << " (" << type.header.filename().string()
      << ")\n";
  switch (type.kind)
  {
  case TypeKind::Component:
    out << "    TypeInfo type = componentType<" << type.qualifiedName << ">("
        << name << ", " << literal(displayName(type.name, true)) << ", " << module
        << ");\n";
    break;
  case TypeKind::MeshSource:
    out << "    static_assert(std::is_base_of_v<sfs::MeshSource, " << type.qualifiedName
        << ">,\n                  "
        << literal(type.name + " must derive from sfs::MeshSource") << ");\n";
    // Scene3D asks every mesh source for its meshes when it is created.
    out << "    {\n      sfs::ScriptType script;\n"
        << "      script.name = " << name << ";\n"
        << "      script.module = " << module << ";\n"
        << "      script.kind = sfs::ScriptType::Kind::MeshSource;\n"
        << "      script.buildMeshes = [](sfs::MeshLibrary& library) {\n"
        << "        " << type.qualifiedName << " source;\n"
        << "        source.build(library);\n"
        << "      };\n"
        << "      sfs::scripts().add(std::move(script));\n    }\n"
        << "    TypeInfo type;\n"
        << "    type.name = " << name << ";\n"
        << "    type.displayName = " << literal(displayName(type.name, false)) << ";\n"
        << "    type.module = " << module << ";\n"
        << "    type.kind = TypeKind::MeshSource;\n";
    break;
  case TypeKind::System:
  case TypeKind::GameObject:
  {
    const bool isSystem = type.kind == TypeKind::System;
    const char* base = isSystem ? "sfs::System" : "sfs::GameObject";
    out << "    static_assert(std::is_base_of_v<" << base << ", "
        << type.qualifiedName << ">,\n                  "
        << literal(type.name + " must derive from " + base) << ");\n";

    // How to instantiate it when a scene plays.
    out << "    {\n      sfs::ScriptType script;\n"
        << "      script.name = " << name << ";\n"
        << "      script.module = " << module << ";\n";
    if (isSystem)
      out << "      script.kind = sfs::ScriptType::Kind::System;\n"
          << "      script.addSystem = [](sfs::Scene& scene) -> sfs::System& {\n"
          << "        return scene.addSystem<" << type.qualifiedName << ">();\n"
          << "      };\n";
    else
      out << "      script.kind = sfs::ScriptType::Kind::GameObject;\n"
          << "      script.createObject = []() -> std::unique_ptr<sfs::GameObject> {\n"
          << "        return std::make_unique<" << type.qualifiedName << ">();\n"
          << "      };\n";
    out << "      sfs::scripts().add(std::move(script));\n    }\n";

    // Reflected too, so the editor can list it and show its fields.
    out << "    TypeInfo type;\n"
        << "    type.name = " << name << ";\n"
        << "    type.displayName = " << literal(displayName(type.name, false))
        << ";\n"
        << "    type.module = " << module << ";\n"
        << "    type.kind = TypeKind::" << (isSystem ? "System" : "GameObject")
        << ";\n";
    break;
  }
  }

  for (const FieldModel& field : type.fields)
    emitField(out, type, field);

  out << "    registry.add(std::move(type));\n  }\n";
}

} // namespace

std::string displayName(std::string_view identifier, bool stripComponentSuffix)
{
  std::string_view base = identifier;
  constexpr std::string_view suffix = "Component";
  if (stripComponentSuffix && base.size() > suffix.size() && base.ends_with(suffix))
    base.remove_suffix(suffix.size());

  std::string out;
  for (std::size_t i = 0; i < base.size(); ++i)
  {
    const auto c = static_cast<unsigned char>(base[i]);
    if (i > 0)
    {
      const auto previous = static_cast<unsigned char>(base[i - 1]);
      const bool nextLower =
          i + 1 < base.size() && std::islower(static_cast<unsigned char>(base[i + 1]));
      // A word starts at lower->Upper ("meshRenderer"), at the last capital of
      // an acronym ("HTTPServer" -> "HTTP Server") and at the first digit of a
      // number ("pointLight3D" -> "Point Light 3D").
      const bool wordStart =
          (std::isupper(c) && (std::islower(previous) ||
                               (std::isupper(previous) && nextLower))) ||
          (std::isdigit(c) && std::isalpha(previous));
      if (wordStart)
        out += ' ';
    }
    out += static_cast<char>(i == 0 ? std::toupper(c) : c);
  }
  return out;
}

std::string generateRegistration(const std::vector<TypeModel>& types,
                                 const GeneratorOptions& options)
{
  std::ostringstream out;
  out << "// Generated by sfs-codegen -- do not edit.\n"
      << "#include \"" << options.functionHeader << "\"\n"
      << "#include \"engine/core/reflection/registration.h\"\n\n";
  // Systems and game objects also register how to instantiate them.
  const bool hasScripts =
      std::any_of(types.begin(), types.end(), [](const TypeModel& type)
                  { return type.kind != TypeKind::Component; });
  if (hasScripts)
    out << "#include \"engine/runtime/rendering3d/meshSource.h\"\n"
        << "#include \"engine/runtime/sceneManager/scene.h\"\n"
        << "#include \"engine/runtime/scripting/scriptRegistry.h\"\n";
  out << "\n";
  for (const TypeModel& type : types)
    out << "#include \"" << includePath(type.header, options.includeRoot) << "\"\n";
  out << "\n#include <cstdint>\n#include <memory>\n#include <string>\n#include <type_traits>\n"
      << "#include <utility>\n\n";

  emitAccessTags(out, types);

  out << "void " << options.function << "(sfs::reflect::TypeRegistry& registry)\n{\n"
      << "  using namespace sfs::reflect;\n\n";
  for (const TypeModel& type : types)
    emitType(out, type, options);
  out << "}\n";

  return out.str();
}

} // namespace sfs::codegen
