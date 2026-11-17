#include "sfs/codegen/generator.h"

#include "sfs/codegen/model.h"
#include "sfs/codegen/rules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
      {"StringList", "std::vector<std::string>"},
      {"Entity", "sfs::Entity"},
      {"EntityList", "std::vector<sfs::Entity>"},
      {"ShaderParameters", "std::vector<sfs::ShaderParameter>"},
  };
  return types;
}

// A field's C++ type, as its static_assert and member pointer spell it.
std::string cppTypeOf(const FieldModel& field)
{
  if (field.kind == "AssetRef" || field.kind == "AssetRefList")
    return "sfs::" + field.kind + "<" + field.assetType + ">";

  return std::string(cppTypeOfKind(field.kind));
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

void emitAccessTags(std::ostringstream& out,
                    const std::vector<TypeModel>& types)
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
      out << "struct " << tag << "\n{\n  using type = " << cppTypeOf(field)
          << " " << type.qualifiedName << "::*;\n  friend type memberOf(" << tag
          << ");\n};\n"
          << "template struct MemberAccess<" << tag << ", &"
          << type.qualifiedName << "::" << field.name << ">;\n\n";
    }
  }

  if (any)
    out << "} // namespace sfs::reflect\n\n";
}

void emitField(std::ostringstream& out,
               const TypeModel& type,
               const FieldModel& field)
{
  const std::string cppType = cppTypeOf(field);

  out << "    {\n";

  if (field.isPublic)
    out << "      static_assert(std::is_same_v<std::remove_cvref_t<decltype("
           "std::declval<"
        << type.qualifiedName << "&>()." << field.name << ")>, " << cppType
        << ">,\n                    "
        << literal(type.name + "::" + field.name + " must be " + cppType)
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

  if (field.texture)
    out << "      field.attributes.texture = true;\n";

  if (field.materials)
    out << "      field.attributes.materials = true;\n";

  if (field.material)
    out << "      field.attributes.material = true;\n";

  if (field.emissionLight)
    out << "      field.attributes.emissionLight = true;\n";

  if (!field.showIf.empty())
    out << "      field.attributes.showIf = \"" << field.showIf << "\";\n";

  if (field.kind == "AssetRef" || field.kind == "AssetRefList")
  {
    // By the registered (unqualified) name: AssetRef<sfs::VehicleSpec> refers
    // to "VehicleSpec".
    const std::size_t colons = field.assetType.rfind("::");
    out << "      field.attributes.assetType = "
        << literal(colons == std::string::npos
                       ? field.assetType
                       : field.assetType.substr(colons + 2))
        << ";\n";
  }

  if (!field.part.empty())
    out << "      field.attributes.part = " << literal(field.part) << ";\n";

  if (field.range)
    out << "      field.attributes.hasRange = true;\n"
        << "      field.attributes.min = static_cast<float>("
        << field.range->first << ");\n"
        << "      field.attributes.max = static_cast<float>("
        << field.range->second << ");\n";

  out << "      field.address = [](void* object) -> void* {\n";

  if (field.isPublic)
    out << "        return &objectAs<" << type.qualifiedName << ">(object)->"
        << field.name << ";\n";
  else
    out << "        return &(objectAs<" << type.qualifiedName
        << ">(object)->*memberOf(" << accessTag(type, field) << "{}));\n";

  out << "      };\n"
      << "      type.fields.push_back(std::move(field));\n"
      << "    }\n";
}

void emitType(std::ostringstream& out,
              const TypeModel& type,
              const GeneratorOptions& options)
{
  const std::string name = literal(type.name);
  const std::string module = literal(options.module);

  out << "  { // " << type.qualifiedName << " ("
      << type.header.filename().string() << ")\n";

  switch (type.kind)
  {
  case TypeKind::Component:
    out << "    TypeInfo type = componentType<" << type.qualifiedName << ">("
        << name << ", " << literal(displayName(type.name, true)) << ", "
        << module << ");\n";
    break;
  case TypeKind::SceneData:
    out << "    TypeInfo type = sceneDataType<" << type.qualifiedName << ">("
        << name << ", " << literal(displayName(type.name, false)) << ", "
        << module << ");\n";
    break;
  case TypeKind::ScriptableObject:
    out << "    static_assert(std::is_base_of_v<sfs::ScriptableObject, "
        << type.qualifiedName << ">,\n                  "
        << literal(type.name + " must derive from sfs::ScriptableObject")
        << ");\n"
        << "    TypeInfo type = scriptableObjectType<" << type.qualifiedName
        << ">(" << name << ", " << literal(displayName(type.name, false))
        << ", " << module << ");\n";
    break;
  case TypeKind::Derived:
    // derivedType's static_assert says what may derive (scriptable objects).
    out << "    TypeInfo type = derivedType<" << type.qualifiedName << ">("
        << name << ", " << literal(displayName(type.name, false)) << ", "
        << module << ", " << literal(type.base) << ");\n";
    break;
  case TypeKind::MeshSource:
    out << "    static_assert(std::is_base_of_v<sfs::MeshSource, "
        << type.qualifiedName << ">,\n                  "
        << literal(type.name + " must derive from sfs::MeshSource") << ");\n";
    // StandardScene asks every mesh source for its meshes when it is created.
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
        << "    type.displayName = " << literal(displayName(type.name, false))
        << ";\n"
        << "    type.module = " << module << ";\n"
        << "    type.kind = TypeKind::MeshSource;\n";
    break;
  case TypeKind::System:
  case TypeKind::Service:
  case TypeKind::GameObject:
  {
    const bool isSystem = type.kind == TypeKind::System;
    const bool isService = type.kind == TypeKind::Service;
    const char* base = isSystem    ? "sfs::System"
                       : isService ? "sfs::Service"
                                   : "sfs::GameObject";
    out << "    static_assert(std::is_base_of_v<" << base << ", "
        << type.qualifiedName << ">,\n                  "
        << literal(type.name + " must derive from " + base) << ");\n";

    // How to instantiate it when a scene plays.
    out << "    {\n      sfs::ScriptType script;\n"
        << "      script.name = " << name << ";\n"
        << "      script.module = " << module << ";\n";

    if (isSystem)
      out << "      script.kind = sfs::ScriptType::Kind::System;\n"
          << "      script.addSystem = [](sfs::Scene& scene) -> sfs::System& "
             "{\n"
          << "        return scene.addSystem<" << type.qualifiedName << ">();\n"
          << "      };\n";
    else if (isService)
      out << "      script.kind = sfs::ScriptType::Kind::Service;\n"
          << "      script.addService = [](sfs::Scene& scene) -> "
             "sfs::Service& {\n"
          << "        return scene.addService<" << type.qualifiedName
          << ">();\n"
          << "      };\n";
    else
    {
      out << "      script.kind = sfs::ScriptType::Kind::GameObject;\n"
          << "      script.createObject = []() -> "
             "std::unique_ptr<sfs::GameObject> {\n"
          << "        return std::make_unique<" << type.qualifiedName
          << ">();\n"
          << "      };\n";

      if (type.uiLayer)
        out << "      script.uiLayer = true;\n";
    }

    out << "      sfs::scripts().add(std::move(script));\n    }\n";

    // Reflected too, so the editor can list it and show its fields.
    out << "    TypeInfo type;\n"
        << "    type.name = " << name << ";\n"
        << "    type.displayName = " << literal(displayName(type.name, false))
        << ";\n"
        << "    type.module = " << module << ";\n"
        << "    type.kind = TypeKind::"
        << (isSystem    ? "System"
            : isService ? "Service"
                        : "GameObject")
        << ";\n";
    break;
  }
  }

  for (const FieldModel& field : type.fields)
    emitField(out, type, field);

  out << "    registry.add(std::move(type));\n  }\n";
}

} // namespace

std::string_view cppTypeOfKind(std::string_view kind)
{
  const auto it = canonicalTypes().find(kind);
  return it != canonicalTypes().end() ? std::string_view(it->second)
                                      : std::string_view();
}

std::string displayName(std::string_view identifier, bool stripComponentSuffix)
{
  std::string_view base = identifier;
  constexpr std::string_view SUFFIX = "Component";

  if (stripComponentSuffix && base.size() > SUFFIX.size() &&
      base.ends_with(SUFFIX))
    base.remove_suffix(SUFFIX.size());

  std::string out;

  for (std::size_t i = 0; i < base.size(); ++i)
  {
    const auto c = static_cast<unsigned char>(base[i]);

    if (i > 0)
    {
      const auto previous = static_cast<unsigned char>(base[i - 1]);
      const bool nextLower =
          i + 1 < base.size() &&
          std::islower(static_cast<unsigned char>(base[i + 1]));
      // A word starts at lower->Upper ("meshRenderer"), at the last capital of
      // an acronym ("HTTPServer" -> "HTTP Server") and at the first digit of a
      // number ("pointLight3D" -> "Point Light 3D").
      const bool wordStart =
          (std::isupper(c) &&
           (std::islower(previous) || (std::isupper(previous) && nextLower))) ||
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
  // Systems, services and game objects also register how to instantiate
  // them.
  const bool hasScripts =
      std::any_of(types.begin(),
                  types.end(),
                  [](const TypeModel& type)
                  {
                    return type.kind == TypeKind::System ||
                           type.kind == TypeKind::Service ||
                           type.kind == TypeKind::GameObject ||
                           type.kind == TypeKind::MeshSource;
                  });

  if (hasScripts)
    out << "#include \"engine/runtime/rendering/meshSource.h\"\n"
        << "#include \"engine/runtime/sceneManager/scene.h\"\n"
        << "#include \"engine/runtime/scripting/scriptRegistry.h\"\n";

  out << "\n";
  // A header declaring several types is included once.
  std::vector<const std::filesystem::path*> headers;

  for (const TypeModel& type : types)
  {
    if (std::any_of(headers.begin(),
                    headers.end(),
                    [&](const std::filesystem::path* header)
                    { return *header == type.header; }))
      continue;

    headers.push_back(&type.header);
    out << "#include \"" << includePath(type.header, options.includeRoot)
        << "\"\n";
  }

  out << "\n#include <cstdint>\n#include <memory>\n#include <string>\n#include "
         "<type_traits>\n"
      << "#include <utility>\n#include <vector>\n\n";

  emitAccessTags(out, types);

  out << "void " << options.function
      << "(sfs::reflect::TypeRegistry& registry)\n{\n"
      << "  using namespace sfs::reflect;\n\n";

  for (const TypeModel& type : types)
    emitType(out, type, options);

  out << "}\n";

  return out.str();
}

std::string generateUiPreviews(const std::vector<UiTypeModel>& types,
                               const GeneratorOptions& options)
{
  std::ostringstream out;
  out << "// Generated by sfs-codegen -- do not edit.\n"
      << "// The project's UI views as the editor's UI Preview shows them: "
         "each filled\n// with placeholders (see "
         "engine/runtime/ui/placeholder.h).\n"
      << "#include \"engine/runtime/scripting/gameModule.h\"\n"
      << "#include \"engine/runtime/ui/placeholder.h\"\n"
      << "#include \"engine/runtime/ui/preview.h\"\n\n";
  std::vector<std::filesystem::path> headers;

  for (const UiTypeModel& type : types)
    if (std::find(headers.begin(), headers.end(), type.header) == headers.end())
      headers.push_back(type.header);

  for (const std::filesystem::path& header : headers)
    out << "#include \"" << includePath(header, options.includeRoot) << "\"\n";

  out << "\n#include <string_view>\n\n";

  if (!types.empty())
  {
    // Declared first, so one type's fields can be lists of another's.
    out << "namespace sfs::ui\n{\n\n";

    for (const UiTypeModel& type : types)
      out << "template <>\nstruct Placeholder<" << type.qualifiedName
          << ">\n{\n  static void fill(" << type.qualifiedName
          << "& value, std::string_view name, int item);\n};\n\n";

    for (const UiTypeModel& type : types)
    {
      out << "void Placeholder<" << type.qualifiedName << ">::fill("
          << type.qualifiedName << "& value, std::string_view, int item)\n{\n";

      if (type.fields.empty())
        out << "  (void)value;\n  (void)item;\n";

      for (const std::string& field : type.fields)
        out << "  placeholder(value." << field << ", "
            << literal(displayName(field, false)) << ", item);\n";

      out << "}\n\n";
    }

    out << "} // namespace sfs::ui\n\n";
  }

  out << "namespace\n{\n";

  for (const UiTypeModel& type : types)
    if (type.view)
      out << "const sfs::ui::Preview preview_" << identifier(type.qualifiedName)
          << "{\n    " << literal(type.name)
          << ",\n    [] { return sfs::ui::placeholderView<"
          << type.qualifiedName << ">(); },\n    std::string("
          << literal(type.header.generic_string()) << ")};\n";

  out << "} // namespace\n\n"
      << "// Nothing to register: the previews are made as it loads.\n"
      << "void " << options.function << "(sfs::reflect::TypeRegistry&) {}\n";
  return out.str();
}

namespace
{

const char* kindName(EventKind kind)
{
  switch (kind)
  {
  case EventKind::News:
    return "News";
  case EventKind::EntityNews:
    return "EntityNews";
  case EventKind::Request:
    return "Request";
  case EventKind::Response:
    return "Response";
  }

  return "News";
}

const char* attributeOf(EventKind kind)
{
  switch (kind)
  {
  case EventKind::News:
    return "[[sfs::event_news]]";
  case EventKind::EntityNews:
    return "[[sfs::event_entity_news]]";
  case EventKind::Request:
    return "[[sfs::event_request(...)]]";
  case EventKind::Response:
    return "[[sfs::event_response]]";
  }

  return "";
}

// The parts of the namespaces `scopes` names, outermost first: a nested
// one's (`a::b`) apart, `inline` dropped; an unnamed one is "".
std::vector<std::string_view> scopeParts(const std::vector<std::string>& scopes)
{
  std::vector<std::string_view> parts;
  parts.reserve(scopes.size());

  for (const std::string& scope : scopes)
  {
    std::string_view rest = scope;

    do
    {
      const std::size_t colons = rest.find("::");
      std::string_view part = rest.substr(0, colons);

      if (part.starts_with("inline "))
        part.remove_prefix(7);

      parts.push_back(part);
      rest = colons == std::string_view::npos ? std::string_view{}
                                              : rest.substr(colons + 2);
    } while (!rest.empty());
  }

  return parts;
}

// Whether the namespaces `outer` contain those of `inner`.
bool encloses(const std::vector<std::string>& outer,
              const std::vector<std::string>& inner)
{
  const std::vector<std::string_view> a = scopeParts(outer);
  const std::vector<std::string_view> b = scopeParts(inner);
  return a.size() <= b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// The qualified name a using name declares at its namespace, e.g.
// "z::Found" for `namespace z { using a::Found; }`.
std::string declaredAt(const UsingName& name)
{
  std::string out;

  for (const std::string_view part : scopeParts(name.scopes))
    if (!part.empty())
    {
      out += part;
      out += "::";
    }

  return out + name.name;
}

} // namespace

std::vector<std::string>
lookupCandidates(std::string_view written,
                 const std::vector<std::string>& scopes)
{
  if (written.starts_with("::"))
    return {std::string(written.substr(2))};

  std::vector<std::string_view> named;

  for (const std::string_view part : scopeParts(scopes))
    if (!part.empty())
      named.push_back(part);

  std::vector<std::string> out;
  out.reserve(named.size() + 1);

  for (std::size_t depth = named.size() + 1; depth-- > 0;)
  {
    std::string name;

    for (std::size_t i = 0; i < depth; ++i)
    {
      name += named[i];
      name += "::";
    }

    name += written;
    out.push_back(std::move(name));
  }

  return out;
}

NameLookup lookUpName(std::string_view spelling,
                      const std::vector<std::string>& scopes,
                      uint32_t offset,
                      std::span<const UsingName> usings)
{
  NameLookup lookup;
  std::string written(spelling);
  const std::vector<std::string>* from = &scopes;
  uint32_t at = offset;
  const auto sees = [&](const UsingName& name)
  { return name.offset < at && at < name.end && encloses(name.scopes, *from); };

  for (int depth = 0; depth < 8; ++depth)
  {
    const std::size_t colons = written.find("::");
    const std::string_view first = std::string_view(written).substr(0, colons);
    const std::vector<std::string> names = lookupCandidates(written, *from);
    const UsingName* named = nullptr;

    for (const UsingName& name : usings)
    {
      if (name.name.empty() || name.offset >= at)
        continue;

      if (name.isNamespace)
      {
        if (colons != std::string::npos && name.name == first && sees(name))
          named = &name;
      }
      else if (name.name == written
                   ? sees(name)
                   : name.end == UINT32_MAX &&
                         std::find(names.begin(),
                                   names.end(),
                                   declaredAt(name)) != names.end())
        named = &name;
    }

    if (!named)
      break;

    written = named->isNamespace ? named->target + written.substr(colons)
                                 : named->target;
    from = &named->scopes;
    at = named->offset;
    lookup.followed = true;
  }

  lookup.candidates = lookupCandidates(written, *from);

  for (const UsingName& directive : usings)
    if (directive.isNamespace && directive.name.empty() && sees(directive))
    {
      for (std::string& name : lookupCandidates(
               directive.target + "::" + written, directive.scopes))
        lookup.candidates.push_back(std::move(name));

      lookup.directed = true;
    }

  return lookup;
}

namespace
{

// Declares a type from outside every namespace, as a traits header does
// before it names the type.
void declare(std::ostringstream& out,
             const std::vector<std::string>& scopes,
             std::string_view classKey,
             std::string_view name)
{
  for (const std::string& scope : scopes)
    out << "namespace " << (scope.empty() ? "" : scope + " ") << "{ ";

  out << classKey << ' ' << name << ';';

  for (std::size_t i = 0; i < scopes.size(); ++i)
    out << " }";

  out << '\n';
}

} // namespace

std::string generateSceneDataTraits(const std::vector<TypeModel>& types)
{
  std::vector<const TypeModel*> marked;

  for (const TypeModel& type : types)
    if (type.kind == TypeKind::SceneData &&
        std::none_of(marked.begin(),
                     marked.end(),
                     [&](const TypeModel* seen)
                     { return seen->qualifiedName == type.qualifiedName; }))
      marked.push_back(&type);

  if (marked.empty())
    return {};

  std::ostringstream out;
  out << "\n// From [[sfs::scene_data]]: what the compiler checks each scene "
         "data type\n// against (engine/core/ecs/sceneData.h).\n"
         "#include <engine/core/ecs/sceneData.h>\n\n";

  for (const TypeModel* type : marked)
    declare(out, type->scopes, type->classKey, type->name);

  out << "\nnamespace sfs::detail\n{\n";

  for (const TypeModel* type : marked)
    out << "template <>\nstruct SceneDataTraits<::" << type->qualifiedName
        << ">\n{\n  static constexpr bool marked = true;\n};\n";

  out << "} // namespace sfs::detail\n";
  return out.str();
}

std::string generateEventTraits(const std::vector<EventModel>& events,
                                const std::vector<std::string>& plainTypes,
                                std::vector<Diagnostic>& diagnostics)
{
  // One entry per qualified name: the same type seen twice is one type.
  std::vector<const EventModel*> unique;
  std::map<std::string, std::size_t, std::less<>> byName;

  for (const EventModel& event : events)
  {
    const auto [at, added] = byName.emplace(event.qualifiedName, unique.size());

    if (added)
    {
      unique.push_back(&event);
      continue;
    }

    const EventModel& first = *unique[at->second];

    if (first.kind != event.kind || first.success != event.success ||
        first.progress != event.progress || first.failure != event.failure ||
        first.bases != event.bases)
      diagnostics.push_back(
          {RuleId::EventDeclaration,
           event.header,
           event.nameRange,
           "The event " + event.qualifiedName +
               " is declared twice, differently. Give each its own name."});
  }

  // The names a type written in an event's declaration may mean, through
  // the namespaces and using names the declaration sees.
  const auto lookUp = [](const std::string& written, const EventModel& from)
  { return lookUpName(written, from.scopes, from.offset, from.usings); };
  const auto find = [&](const NameLookup& lookup) -> const EventModel*
  {
    for (const std::string& name : lookup.candidates)
      if (const auto found = byName.find(name); found != byName.end())
        return unique[found->second];

    return nullptr;
  };
  const auto isPlain = [&](const NameLookup& lookup)
  {
    for (const std::string& name : lookup.candidates)
      if (std::find(plainTypes.begin(), plainTypes.end(), name) !=
          plainTypes.end())
        return true;

    return false;
  };
  const auto report = [&](const EventModel& event, std::string message)
  {
    diagnostics.push_back({RuleId::EventDeclaration,
                           event.header,
                           event.nameRange,
                           std::move(message)});
  };
  // A type a traits header names, from outside every namespace. One no
  // scanned file declares is the engine's (or the compiler reports it),
  // named as the request's namespaces would; but where a using name decides
  // what it is, that can't be told, and it's reported.
  const auto spelled = [&](const std::string& written,
                           const EventModel& from) -> std::string
  {
    const NameLookup lookup = lookUp(written, from);

    if (const EventModel* found = find(lookup))
      return "::" + found->qualifiedName;

    if (lookup.followed ||
        (lookup.directed && written.find("::") == std::string::npos))
      report(from,
             from.name + " lists " + written +
                 ", which a using-declaration, alias or using-directive names "
                 "but no scanned header declares, so its traits can't name "
                 "it. Write its full name, with its namespaces.");

    // A qualified name (`sfs::PathFound`) is taken from the global
    // namespace, an unqualified one from the request's.
    if (!lookup.followed && written.find("::") != std::string::npos &&
        !written.starts_with("::"))
      return "::" + written;

    return "::" + lookup.candidates.front();
  };

  std::ostringstream out;
  out << "// Generated by sfs-codegen from the event attributes: what the "
         "compiler\n// checks each event type against "
         "(engine/core/events/eventTraits.h).\n// Every file of the target "
         "includes it first: never include it by hand.\n#pragma once\n"
         "// IWYU pragma: private, include "
         "\"engine/core/events/eventTraits.h\"\n\n"
         "#include <engine/core/events/eventTraits.h>\n";

  if (unique.empty())
    return out.str();

  out << '\n';

  for (const EventModel* event : unique)
    declare(out, event->scopes, event->classKey, event->name);

  out << "\nnamespace sfs::detail\n{\n";

  for (const EventModel* event : unique)
  {
    // A request's responses by role, in the order success, progress,
    // failure: each a response type, and each type in one role.
    struct Role
    {
      const char* name;  // as the attribute writes it
      const char* trait; // the traits' alias
      const std::string* written;
      std::string spelled;
    };
    std::array<Role, 3> roles = {
        {{"success", "Success", &event->success, {}},
         {"progress", "Progress", &event->progress, {}},
         {"failure", "Failure", &event->failure, {}}}};

    for (Role& role : roles)
    {
      if (role.written->empty())
        continue;

      const std::string& written = *role.written;
      const NameLookup lookup = lookUp(written, *event);
      const EventModel* response = find(lookup);

      if ((response && response->kind != EventKind::Response) ||
          (!response && isPlain(lookup)))
        report(*event,
               event->name + " names " + written + " as its " + role.name +
                   " response, but " + written +
                   " isn't marked [[sfs::event_response]]. A request's "
                   "roles name only response types: mark it.");

      role.spelled = spelled(written, *event);

      for (const Role& other : roles)
        if (&other != &role && other.spelled == role.spelled)
        {
          report(*event,
                 event->name + " names " + written + " as its " + other.name +
                     " and its " + role.name +
                     " response, but a type plays one role in a request. "
                     "Give each role a type of its own.");
          break;
        }
    }

    // Its event ancestors, nearest first, each once.
    std::vector<const EventModel*> ancestors;
    std::vector<const EventModel*> pending{event};

    for (std::size_t next = 0; next < pending.size(); ++next)
      for (const EventBase& written : pending[next]->bases)
      {
        const EventModel* base = find(lookUp(written.name, *pending[next]));

        if (!base || base == event ||
            std::find(ancestors.begin(), ancestors.end(), base) !=
                ancestors.end())
          continue;

        ancestors.push_back(base);
        pending.push_back(base);
      }

    // A cycle of bases (`A : B` and `B : A`, or `Self : Self`) isn't valid
    // C++; one through the event is reported on it. With a cycle anywhere
    // among its bases there are no copies to count.
    bool loops = false;
    bool throughEvent = false;
    std::vector<std::pair<const EventModel*, bool>> seen; // type, finished
    const std::function<void(const EventModel*)> visit =
        [&](const EventModel* at)
    {
      seen.emplace_back(at, false);

      for (const EventBase& written : at->bases)
      {
        const EventModel* base = find(lookUp(written.name, *at));

        if (!base)
          continue;

        const auto found =
            std::find_if(seen.begin(),
                         seen.end(),
                         [&](const auto& mark) { return mark.first == base; });

        if (found == seen.end())
          visit(base);
        else if (!found->second)
        {
          loops = true;
          throughEvent = throughEvent || base == event;
        }
      }

      for (auto& mark : seen)
        if (mark.first == at)
          mark.second = true;
    };
    visit(event);

    if (throughEvent)
      report(*event,
             event->name + " derives from itself through its event bases, "
                           "which C++ doesn't allow. Remove the base that "
                           "closes the loop.");

    // How many copies of each ancestor the event holds, as C++ lays them
    // out: one per path of plain bases, plus one shared by every path that
    // reaches it as a virtual base. More than one makes the cast to send the
    // event as that ancestor ambiguous.
    std::vector<const EventModel*> copies;
    std::vector<const EventModel*> shared;
    std::vector<const EventModel*> walk;

    if (!loops)
      walk.push_back(event);

    while (!walk.empty())
    {
      const EventModel* from = walk.back();
      walk.pop_back();

      for (const EventBase& written : from->bases)
      {
        const EventModel* base = find(lookUp(written.name, *from));

        if (!base)
          continue;

        if (!written.isVirtual)
          copies.push_back(base);
        else if (std::find(shared.begin(), shared.end(), base) != shared.end())
          continue;
        else
          shared.push_back(base);

        walk.push_back(base);
      }
    }

    for (const EventModel* base : ancestors)
    {
      const auto count = std::count(copies.begin(), copies.end(), base) +
                         std::count(shared.begin(), shared.end(), base);

      if (count > 1)
        report(*event,
               event->name + " reaches the event base " + base->name +
                   " twice, through two of its bases, so it can't be sent as "
                   "one " +
                   base->name + ". Derive from " + base->name +
                   " along one path only, or as a virtual base on every "
                   "path.");
    }

    for (const EventModel* base : ancestors)
    {
      const bool outside = event->kind == EventKind::Request ||
                           event->kind == EventKind::Response ||
                           base->kind == EventKind::Request ||
                           base->kind == EventKind::Response;

      if (outside)
        report(*event,
               event->name + " derives from " + base->name +
                   ", but requests and responses take no part in event "
                   "families. Give " +
                   event->name + " the fields itself.");
      else if (base->kind != event->kind)
        report(*event,
               event->name + " is " + attributeOf(event->kind) +
                   " but derives from " + base->name + ", which is " +
                   attributeOf(base->kind) +
                   ". An event is the kind its event bases are.");
    }

    out << "template <>\nstruct EventTraits<::" << event->qualifiedName
        << ">\n{\n  static constexpr EventKind kind = EventKind::"
        << kindName(event->kind) << ";\n";

    if (event->kind == EventKind::Request)
      for (const Role& role : roles)
        out << "  using " << role.trait << " = "
            << (role.spelled.empty() ? "void" : role.spelled) << ";\n";

    out << "  using Ancestors = TypeList<";

    for (std::size_t i = 0; i < ancestors.size(); ++i)
      out << (i ? ", " : "") << "::" << ancestors[i]->qualifiedName;

    out << ">;\n};\n";
  }

  out << "} // namespace sfs::detail\n";
  return out.str();
}

} // namespace sfs::codegen
