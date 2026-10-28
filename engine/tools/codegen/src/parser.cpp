#include "sfs/codegen/parser.h"

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-cpp.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sfs::codegen
{

namespace
{

// Field types the editor can edit, by their (normalised) spelling.
const std::map<std::string, std::string, std::less<>>& fieldKinds()
{
  static const std::map<std::string, std::string, std::less<>> kinds = {
      {"bool", "Bool"},
      {"int", "Int"},
      {"int32_t", "Int"},
      {"std::int32_t", "Int"},
      {"unsigned", "UInt"},
      {"unsigned int", "UInt"},
      {"uint32_t", "UInt"},
      {"std::uint32_t", "UInt"},
      {"float", "Float"},
      {"std::string", "String"},
      {"glm::vec2", "Vec2"},
      {"glm::vec3", "Vec3"},
      {"glm::vec4", "Vec4"},
      {"glm::quat", "Quat"},
      {"MeshHandle", "Mesh"},
      {"sfs::MeshHandle", "Mesh"},
      {"nlohmann::json", "Json"},
      {"std::vector<std::string>", "StringList"},
  };
  return kinds;
}

std::string lowercase(std::string_view text)
{
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
                 { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Collapse runs of whitespace to one space and drop spaces around "::".
// T, for a (normalised) "sfs::AssetRef<T>" or "AssetRef<T>".
std::optional<std::string> assetRefType(std::string_view type)
{
  if (type.starts_with("sfs::"))
    type.remove_prefix(5);
  constexpr std::string_view open = "AssetRef<";
  if (!type.starts_with(open) || !type.ends_with('>'))
    return std::nullopt;
  std::string inner(type.substr(open.size(), type.size() - open.size() - 1));
  while (!inner.empty() && inner.front() == ' ')
    inner.erase(inner.begin());
  while (!inner.empty() && inner.back() == ' ')
    inner.pop_back();
  return inner.empty() ? std::nullopt : std::optional(inner);
}

std::string normaliseType(std::string_view text)
{
  std::string out;
  bool pendingSpace = false;
  for (const char c : text)
  {
    if (std::isspace(static_cast<unsigned char>(c)))
    {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace && c != ':' && (out.empty() || out.back() != ':'))
      out += ' ';
    pendingSpace = false;
    out += c;
  }
  return out;
}

// Walks one header's syntax tree.
class HeaderParser
{
public:
  HeaderParser(std::string_view source, const std::filesystem::path& path)
      : m_source(source), m_path(path),
        m_target(lowercase(path.stem().string()))
  {
  }

  ParseResult run()
  {
    const std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(
        ts_parser_new(), &ts_parser_delete);
    ts_parser_set_language(parser.get(), tree_sitter_cpp());

    const std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(
        ts_parser_parse_string(parser.get(),
                               nullptr,
                               m_source.data(),
                               static_cast<uint32_t>(m_source.size())),
        &ts_tree_delete);

    visit(ts_tree_root_node(tree.get()));

    if (!m_result.type && !m_helper)
      warn(ts_tree_root_node(tree.get()),
           "no class or struct named after the file ('" +
               m_path.stem().string() + "')");
    return std::move(m_result);
  }

private:
  std::string text(TSNode node) const
  {
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    return std::string(m_source.substr(start, end - start));
  }

  static std::string type(TSNode node) { return ts_node_type(node); }

  static TSNode field(TSNode node, const char* name)
  {
    return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(std::strlen(name)));
  }

  void report(TSNode node, Diagnostic::Severity severity, std::string message)
  {
    m_result.diagnostics.push_back(
        {severity, m_path, ts_node_start_point(node).row + 1, std::move(message)});
  }
  void warn(TSNode node, std::string message)
  {
    report(node, Diagnostic::Severity::Warning, std::move(message));
  }
  void error(TSNode node, std::string message)
  {
    report(node, Diagnostic::Severity::Error, std::move(message));
  }

  // Depth-first over declarations, tracking the enclosing namespaces.
  void visit(TSNode node)
  {
    const std::string kind = type(node);

    if (kind == "namespace_definition")
    {
      const TSNode name = field(node, "name");
      const std::size_t depth = m_namespaces.size();
      if (!ts_node_is_null(name))
        m_namespaces.push_back(text(name)); // "a" or nested "a::b"
      visitChildren(node);
      m_namespaces.resize(depth);
      return;
    }

    if ((kind == "class_specifier" || kind == "struct_specifier") &&
        !ts_node_is_null(field(node, "body")))
    {
      const TSNode name = field(node, "name");
      if (!ts_node_is_null(name) && lowercase(text(name)) == m_target)
      {
        if (m_result.type)
          error(node, "more than one type named '" + text(name) + "'");
        else
          parseType(node, kind == "struct_specifier");
      }
      return; // nested types are never the script type
    }

    visitChildren(node);
  }

  void visitChildren(TSNode node)
  {
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
      visit(ts_node_named_child(node, i));
  }

  void parseType(TSNode node, bool isStruct)
  {
    TypeModel model;
    model.name = text(field(node, "name"));
    model.header = m_path;
    for (const std::string& ns : m_namespaces)
      model.qualifiedName += ns + "::";
    model.qualifiedName += model.name;
    model.kind = classify(node);

    // struct members default to public, class members to private.
    bool isPublic = isStruct;
    const TSNode body = field(node, "body");
    const uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode member = ts_node_named_child(body, i);
      const std::string memberKind = type(member);
      if (memberKind == "access_specifier")
        isPublic = text(member) == "public";
      else if (memberKind == "field_declaration")
        parseField(member, isPublic, model);
    }

    // A class with no engine base and nothing to edit (e.g. FieldMap, a
    // helper a script uses) isn't a component: it's left alone. Components
    // are structs, or classes with public fields.
    if (!isStruct && model.kind == TypeKind::Component && model.fields.empty())
    {
      m_helper = true;
      return;
    }
    m_result.type = std::move(model);
  }

  TypeKind classify(TSNode node)
  {
    TypeKind kind = TypeKind::Component;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);
      if (type(child) != "base_class_clause")
        continue;

      const uint32_t bases = ts_node_named_child_count(child);
      for (uint32_t b = 0; b < bases; ++b)
      {
        const TSNode base = ts_node_named_child(child, b);
        if (type(base) == "access_specifier")
          continue;
        const std::string name = normaliseType(text(base));
        if (name == "GameObject" || name == "sfs::GameObject")
          kind = TypeKind::GameObject;
        else if (name == "System" || name == "sfs::System")
          kind = TypeKind::System;
        else if (name == "MeshSource" || name == "sfs::MeshSource")
          kind = TypeKind::MeshSource;
        else if (name == "ScriptableObject" || name == "sfs::ScriptableObject")
          kind = TypeKind::ScriptableObject;
        else
          error(base,
                "'" + name + "' isn't a supported base; derive from sfs::GameObject, "
                "sfs::System, sfs::MeshSource or sfs::ScriptableObject, or nothing for a component");
      }
    }
    return kind;
  }

  void parseField(TSNode node, bool isPublic, TypeModel& model)
  {
    bool hide = false;
    bool show = false;
    FieldModel base;
    base.isPublic = isPublic;

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);
      const std::string childKind = type(child);

      if (childKind == "storage_class_specifier" && text(child) == "static")
        return; // static members aren't per-object state
      if (childKind == "type_qualifier" && text(child) == "const")
        return; // nothing to edit
      if (childKind == "attribute_declaration")
        parseAttributes(child, base, hide, show);
    }

    // Methods declared without a body are field_declarations too, the
    // function declarator inside any pointer or reference to what they
    // return (`Thing* find();`).
    TSNode declarator = field(node, "declarator");
    while (!ts_node_is_null(declarator) &&
           (type(declarator) == "pointer_declarator" || type(declarator) == "reference_declarator"))
    {
      const TSNode inner = field(declarator, "declarator");
      declarator = ts_node_is_null(inner) && ts_node_named_child_count(declarator) > 0
                       ? ts_node_named_child(declarator, ts_node_named_child_count(declarator) - 1)
                       : inner;
    }
    if (ts_node_is_null(declarator) || type(declarator) == "function_declarator")
      return;

    if (hide || (!isPublic && !show))
      return;

    const TSNode typeNode = field(node, "type");
    base.type = normaliseType(text(typeNode));
    // A reference to a scriptable object, sfs::AssetRef<T>; otherwise a fixed type.
    std::optional<std::string> kind;
    if (const std::optional<std::string> data = assetRefType(base.type))
    {
      kind = "AssetRef";
      base.assetType = *data;
    }
    else if (const auto known = fieldKinds().find(base.type); known != fieldKinds().end())
      kind = known->second;

    // Every declarator: `float a, b;` declares two fields.
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);
      const char* fieldName = ts_node_field_name_for_named_child(node, i);
      if (!fieldName || std::string_view(fieldName) != "declarator")
        continue;

      if (type(child) != "field_identifier")
      {
        warn(child, "'" + text(child) + "': pointer, reference and array "
                    "fields can't be edited; skipped");
        continue;
      }
      if (!kind)
      {
        warn(child, "'" + text(child) + "' has type '" + base.type +
                        "', which the editor can't edit yet; skipped");
        continue;
      }

      FieldModel fieldModel = base;
      fieldModel.name = text(child);
      fieldModel.kind = *kind;
      model.fields.push_back(std::move(fieldModel));
    }
  }

  void parseAttributes(TSNode declaration, FieldModel& fieldModel, bool& hide, bool& show)
  {
    const uint32_t count = ts_node_named_child_count(declaration);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode attribute = ts_node_named_child(declaration, i);
      const TSNode prefix = field(attribute, "prefix");
      if (ts_node_is_null(prefix) || text(prefix) != "sfs")
        continue; // someone else's attribute

      const std::string name = text(field(attribute, "name"));
      if (name == "hide")
        hide = true;
      else if (name == "show")
        show = true;
      else if (name == "color")
        fieldModel.color = true;
      else if (name == "script")
        fieldModel.script = true;
      else if (name == "texture")
        fieldModel.texture = true;
      else if (name == "materials")
        fieldModel.materials = true;
      else if (name == "range")
        parseRange(attribute, fieldModel);
      else
        warn(attribute, "unknown attribute [[sfs::" + name + "]]");
    }
  }

  void parseRange(TSNode attribute, FieldModel& fieldModel)
  {
    TSNode arguments{};
    const uint32_t count = ts_node_named_child_count(attribute);
    for (uint32_t i = 0; i < count; ++i)
      if (type(ts_node_named_child(attribute, i)) == "argument_list")
        arguments = ts_node_named_child(attribute, i);

    if (ts_node_is_null(arguments) || ts_node_named_child_count(arguments) != 2)
    {
      warn(attribute, "[[sfs::range]] needs two arguments: (min, max)");
      return;
    }
    fieldModel.range = {text(ts_node_named_child(arguments, 0)),
                        text(ts_node_named_child(arguments, 1))};
  }

  std::string_view m_source;
  std::filesystem::path m_path;
  std::string m_target;
  std::vector<std::string> m_namespaces;
  ParseResult m_result;
  bool m_helper = false; // the file's class is a helper (no type, no warning)
};

} // namespace

ParseResult parseHeader(std::string_view source, const std::filesystem::path& path)
{
  return HeaderParser(source, path).run();
}

} // namespace sfs::codegen
