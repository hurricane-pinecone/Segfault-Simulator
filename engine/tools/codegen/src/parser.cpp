#include "sfs/codegen/parser.h"

#include "sfs/codegen/attributes.h"
#include "sfs/codegen/suppressions.h"

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-cpp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
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
      {"bool", "Bool"},           {"int", "Int"},
      {"int32_t", "Int"},         {"std::int32_t", "Int"},
      {"unsigned", "UInt"},       {"unsigned int", "UInt"},
      {"uint32_t", "UInt"},       {"std::uint32_t", "UInt"},
      {"float", "Float"},         {"std::string", "String"},
      {"glm::vec2", "Vec2"},      {"glm::vec3", "Vec3"},
      {"glm::vec4", "Vec4"},      {"glm::quat", "Quat"},
      {"MeshHandle", "Mesh"},     {"sfs::MeshHandle", "Mesh"},
      {"nlohmann::json", "Json"}, {"std::vector<std::string>", "StringList"},
      {"sfs::Entity", "Entity"},  {"std::vector<sfs::Entity>", "EntityList"},
  };
  return kinds;
}

// Spellings of the entity kinds that name sfs::Entity only inside namespace
// sfs (elsewhere `Entity` may be anything).
const std::map<std::string, std::string, std::less<>>& sfsFieldKinds()
{
  static const std::map<std::string, std::string, std::less<>> kinds = {
      {"Entity", "Entity"},
      {"std::vector<Entity>", "EntityList"},
  };
  return kinds;
}

bool isEntityKind(std::string_view kind)
{
  return kind == "Entity" || kind == "EntityList";
}

std::string lowercase(std::string_view text)
{
  std::string out(text);
  std::transform(out.begin(),
                 out.end(),
                 out.begin(),
                 [](unsigned char c)
                 { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Collapse runs of whitespace to one space and drop spaces around "::".
// T, for a (normalised) "sfs::<wrapper><T>" or "<wrapper><T>" (wrapper:
// "AssetRef", "AssetRefList").
std::optional<std::string> assetRefType(std::string_view type,
                                        std::string_view wrapper)
{
  if (type.starts_with("sfs::"))
    type.remove_prefix(5);
  if (!type.starts_with(wrapper))
    return std::nullopt;
  type.remove_prefix(wrapper.size());
  constexpr std::string_view open = "<";
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

// How deep a walk of the tree has gone. Declarations never nest deeply; only
// generated or hostile code (an expression of thousands of terms) does, and
// following it down would overflow the stack. Below the limit the walk stops,
// which loses nothing codegen reads.
class NestingLimit
{
public:
  static constexpr int kMaxDepth = 256;

  // Goes a level down: false at the limit (logged once per file).
  bool enter(const std::filesystem::path& path)
  {
    if (m_depth >= kMaxDepth)
    {
      if (!m_logged)
        std::fprintf(stderr,
                     "sfs-codegen: %s: code nested over %d levels deep is "
                     "skipped\n",
                     path.string().c_str(),
                     kMaxDepth);
      m_logged = true;
      return false;
    }
    ++m_depth;
    return true;
  }

  void leave() { --m_depth; }

private:
  int m_depth = 0;
  bool m_logged = false;
};

// An attribute's arguments, read by its shape: MinMax both, BoolField the
// field ("!" first when negated), String the unescaped text.
struct AttributeValue
{
  std::string first;
  std::string second;
};

// A field's [[sfs::...]] attribute, read but waiting for the field's kind.
struct FieldAttribute
{
  const AttributeInfo* info;
  TSNode node;
  AttributeValue value;
};

// Walks one header's syntax tree.
class HeaderParser
{
public:
  HeaderParser(std::string_view source, const std::filesystem::path& path)
      : m_source(source), m_path(path),
        m_target(lowercase(path.stem().string()))
  {
  }

  ParseResult run(const TSTree* tree)
  {
    visit(ts_tree_root_node(tree));

    if (!m_result.type && !m_helper && m_result.components.empty())
    {
      // On the first line, so an editor doesn't underline the whole file.
      const std::size_t lineEnd = m_source.find_first_of("\r\n");
      const auto width = static_cast<unsigned>(
          lineEnd == std::string_view::npos ? m_source.size() : lineEnd);
      m_result.diagnostics.push_back(
          {RuleId::NoScriptType,
           m_path,
           {0, 0, 0, width},
           "No class or struct in this header is named after the file, " +
               m_path.stem().string() + ". Name the script type " +
               m_path.stem().string() +
               ", or mark a helper header with "
               "sfs-ignore-file(no-script-type)."});
    }

    m_result.suppressions =
        findSuppressions(tree, m_source, m_path, m_result.diagnostics);
    applySuppressions(m_result.suppressions, m_result.diagnostics);
    return std::move(m_result);
  }

private:
  std::string text(TSNode node) const
  {
    return std::string(nodeText(m_source, node));
  }

  static std::string type(TSNode node) { return ts_node_type(node); }

  static TSNode field(TSNode node, const char* name)
  {
    return childField(node, name);
  }

  // A diagnostic of `rule` about `node`; its severity is the rule's.
  void report(RuleId rule,
              TSNode node,
              std::string message,
              std::string fixTitle = {},
              std::vector<TextEdit> fix = {})
  {
    m_result.diagnostics.push_back({rule,
                                    m_path,
                                    rangeOf(node),
                                    std::move(message),
                                    std::move(fixTitle),
                                    std::move(fix)});
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
      const bool marked = isMarkedComponent(node);
      if (m_classDepth == 0 && !ts_node_is_null(name) &&
          lowercase(text(name)) == m_target)
      {
        if (m_result.type)
          report(RuleId::DuplicateType,
                 node,
                 "More than one type is named " + text(name) +
                     ". Give each type a name of its own.");
        else
          parseType(node, kind == "struct_specifier", marked);
      }
      else if (marked && ts_node_is_null(name))
        report(RuleId::ComponentNotRegistered,
               node,
               "An unnamed type can't be registered as a component. Give it "
               "a name, or remove [[sfs::component]].");
      else if (marked && (m_classDepth > 0 || m_localDepth > 0))
        report(RuleId::ComponentNotRegistered,
               node,
               "The component " + text(name) +
                   " is nested or a template, so it isn't registered. Declare "
                   "it as a plain type at namespace scope.");
      else if (marked)
        parseType(node, kind == "struct_specifier", marked);

      // Nested types are never registered, but a marked one is warned about.
      ++m_classDepth;
      visitChildren(field(node, "body"));
      --m_classDepth;
      return;
    }

    // Types in a template or a function aren't at namespace scope.
    if (kind == "template_declaration" || kind == "function_definition")
    {
      ++m_localDepth;
      visitChildren(node);
      --m_localDepth;
      return;
    }

    visitChildren(node);
  }

  // Whether a class specifier carries [[sfs::component]]; warns about any
  // other [[sfs::...]] attribute on it.
  bool isMarkedComponent(TSNode node)
  {
    bool marked = false;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode declaration = ts_node_named_child(node, i);
      if (type(declaration) != "attribute_declaration")
        continue;
      const uint32_t attributes = ts_node_named_child_count(declaration);
      for (uint32_t a = 0; a < attributes; ++a)
      {
        const TSNode attribute = ts_node_named_child(declaration, a);
        const AttributeInfo* info = sfsAttribute(attribute);
        if (!info)
          continue;
        if (info->target != AttributeTarget::Type)
        {
          wrongTarget(*info, attribute);
          continue;
        }
        AttributeValue value;
        if (attributeArguments(*info, attribute, value) &&
            info->id == AttributeId::Component)
          marked = true;
      }
    }
    return marked;
  }

  // The table entry of an [[sfs::...]] attribute; null for someone else's
  // attribute, or (with a warning) an unknown one.
  const AttributeInfo* sfsAttribute(TSNode attribute)
  {
    const TSNode prefix = field(attribute, "prefix");
    if (ts_node_is_null(prefix) || text(prefix) != "sfs")
      return nullptr;
    const TSNode nameNode = field(attribute, "name");
    const std::string name = text(nameNode);
    if (const AttributeInfo* info = findAttribute(name))
      return info;

    std::string message = "There is no attribute [[sfs::" + name + "]].";
    if (const AttributeInfo* closest = closestAttribute(name))
    {
      const std::string fixed = "[[sfs::" + std::string(closest->name) + "]]";
      message += " Use " + fixed + " if that's what you meant.";
      report(RuleId::UnknownSfsAttribute,
             attribute,
             std::move(message),
             "Change to " + fixed,
             {{rangeOf(nameNode), std::string(closest->name)}});
    }
    else
      report(RuleId::UnknownSfsAttribute, attribute, std::move(message));
    return nullptr;
  }

  void wrongTarget(const AttributeInfo& info, TSNode attribute)
  {
    const bool onType = info.target == AttributeTarget::Type;
    report(RuleId::AttributeTarget,
           attribute,
           "The attribute [[sfs::" + std::string(info.name) + "]] goes on " +
               (onType ? "a type, not a field" : "a field, not a type") +
               ", so it's ignored here. Move it to " +
               (onType ? "the type." : "a field."));
  }

  static TSNode argumentList(TSNode attribute)
  {
    const uint32_t count = ts_node_named_child_count(attribute);
    for (uint32_t i = 0; i < count; ++i)
      if (type(ts_node_named_child(attribute, i)) == "argument_list")
        return ts_node_named_child(attribute, i);
    return TSNode{};
  }

  // Reads an attribute's arguments by its table shape into `value`; false,
  // with a warning, when they don't fit it.
  bool attributeArguments(const AttributeInfo& info,
                          TSNode attribute,
                          AttributeValue& value)
  {
    const TSNode arguments = argumentList(attribute);
    const uint32_t count =
        ts_node_is_null(arguments) ? 0 : ts_node_named_child_count(arguments);
    const TSNode argument =
        count == 1 ? ts_node_named_child(arguments, 0) : TSNode{};
    const std::string written = "[[sfs::" + std::string(info.name) + "]]";
    switch (info.args)
    {
    case AttributeArgs::None:
      // Still applied: the arguments are only noise.
      if (!ts_node_is_null(arguments))
        report(RuleId::AttributeArguments,
               attribute,
               "The attribute " + written +
                   " takes no arguments. Remove the parentheses.");
      return true;
    case AttributeArgs::MinMax:
      if (count != 2)
        break;
      value.first = text(ts_node_named_child(arguments, 0));
      value.second = text(ts_node_named_child(arguments, 1));
      return true;
    case AttributeArgs::BoolField:
    {
      // (field), or (!field) to show it while the field is off.
      const bool negated =
          !ts_node_is_null(argument) && type(argument) == "unary_expression" &&
          ts_node_named_child_count(argument) == 1 &&
          text(argument).starts_with("!") &&
          type(ts_node_named_child(argument, 0)) == "identifier";
      if (ts_node_is_null(argument) ||
          (type(argument) != "identifier" && !negated))
      {
        report(
            RuleId::AttributeArguments,
            attribute,
            "The attribute " + written +
                " needs the name of a bool field. Write (field), or (!field) "
                "for while it's off.");
        return false;
      }
      value.first = negated ? "!" + text(ts_node_named_child(argument, 0))
                            : text(argument);
      return true;
    }
    case AttributeArgs::String:
    {
      const std::string literal =
          ts_node_is_null(argument) || type(argument) != "string_literal"
              ? std::string()
              : text(argument);
      if (literal.size() < 3 || literal.front() != '"' || literal.back() != '"')
      {
        report(RuleId::AttributeArguments,
               attribute,
               "The attribute " + written +
                   " needs one non-empty string. Write " +
                   std::string(info.arguments) + ".");
        return false;
      }
      value.first.clear();
      for (std::size_t i = 1; i + 1 < literal.size(); ++i)
      {
        if (literal[i] == '\\' && i + 2 < literal.size() &&
            (literal[i + 1] == '"' || literal[i + 1] == '\\'))
          ++i;
        value.first += literal[i];
      }
      return true;
    }
    }
    report(RuleId::AttributeArguments,
           attribute,
           "The attribute " + written + " needs two arguments. Write " +
               std::string(info.arguments) + ".");
    return false;
  }

  void visitChildren(TSNode node)
  {
    if (!m_depth.enter(m_path))
      return;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
      visit(ts_node_named_child(node, i));
    m_depth.leave();
  }

  // Registers a class specifier: the file's type, or (marked) one of the
  // header's [[sfs::component]] types.
  void parseType(TSNode node, bool isStruct, bool marked)
  {
    TypeModel model;
    model.name = text(field(node, "name"));
    model.nameRange = rangeOf(field(node, "name"));
    model.header = m_path;
    model.line = ts_node_start_point(node).row + 1;
    for (const std::string& ns : m_namespaces)
      model.qualifiedName += ns + "::";
    model.qualifiedName += model.name;
    model.kind = classify(node, model);

    const bool fileType = lowercase(model.name) == m_target;
    if (marked && model.kind != TypeKind::Component)
    {
      report(RuleId::ComponentBase,
             node,
             "A component can't derive from another type, but " + model.name +
                 " derives from " + baseOf(node) + ". Give " + model.name +
                 " the fields itself, or put the shared data in a component "
                 "of its own.");
      return;
    }
    if (!fileType && registered(model.name))
    {
      report(RuleId::DuplicateType,
             node,
             "More than one type is named " + model.name +
                 ". Give each type a name of its own.");
      return;
    }

    m_showIfs.clear();
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
    checkShowIfs(model);

    // A class with no engine base and nothing to edit (e.g. FieldMap, a
    // helper a script uses) isn't a component: it's left alone. Components
    // are structs, or classes with public fields.
    if (!marked && !isStruct && model.kind == TypeKind::Component &&
        model.fields.empty())
    {
      m_helper = true;
      return;
    }
    if (fileType)
      m_result.type = std::move(model);
    else
      m_result.components.push_back(std::move(model));
  }

  // Whether the header already registers a type with this (unqualified) name.
  bool registered(std::string_view name) const
  {
    if (m_result.type && m_result.type->name == name)
      return true;
    return std::any_of(m_result.components.begin(),
                       m_result.components.end(),
                       [&](const TypeModel& type)
                       { return type.name == name; });
  }

  // A class specifier's first base, as written.
  std::string baseOf(TSNode node) const
  {
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode clause = ts_node_named_child(node, i);
      if (type(clause) != "base_class_clause")
        continue;
      const uint32_t baseCount = ts_node_named_child_count(clause);
      for (uint32_t b = 0; b < baseCount; ++b)
      {
        const TSNode base = ts_node_named_child(clause, b);
        if (type(base) != "access_specifier" && type(base) != "virtual")
          return normaliseType(text(base));
      }
    }
    return {};
  }

  TypeKind classify(TSNode node, TypeModel& model)
  {
    TypeKind kind = TypeKind::Component;
    int bases = 0;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);
      if (type(child) != "base_class_clause")
        continue;

      const uint32_t baseCount = ts_node_named_child_count(child);
      for (uint32_t b = 0; b < baseCount; ++b)
      {
        const TSNode base = ts_node_named_child(child, b);
        if (type(base) == "access_specifier" || type(base) == "virtual")
          continue;
        const std::string name = normaliseType(text(base));
        if (++bases > 1)
        {
          report(RuleId::ScriptBase,
                 base,
                 "A script type has one base, so " + name +
                     " can't be another. Derive from one engine base or one "
                     "reflected type.");
          continue;
        }
        // A UI layer is a game object (drawn as UI).
        if (name == "GameObject" || name == "sfs::GameObject")
          kind = TypeKind::GameObject;
        else if (name == "ui::Layer" || name == "sfs::ui::Layer")
        {
          kind = TypeKind::GameObject;
          model.uiLayer = true;
        }
        else if (name == "System" || name == "sfs::System")
          kind = TypeKind::System;
        else if (name == "MeshSource" || name == "sfs::MeshSource")
          kind = TypeKind::MeshSource;
        else if (name == "ScriptableObject" || name == "sfs::ScriptableObject")
          kind = TypeKind::ScriptableObject;
        else if (name.find_first_of("<>(") == std::string::npos)
        {
          // Another reflected type (sfs::VehicleSpec, game::ItemDef): its
          // fields come first; the compiler checks what it is.
          kind = TypeKind::Derived;
          const std::size_t colons = name.rfind("::");
          model.base =
              colons == std::string::npos ? name : name.substr(colons + 2);
        }
        else
          report(RuleId::ScriptBase,
                 base,
                 name + " isn't a base a script type can have. Derive from "
                        "sfs::GameObject, sfs::ui::Layer, sfs::System, "
                        "sfs::MeshSource, sfs::ScriptableObject or another "
                        "reflected type, or from nothing for a component.");
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
    m_fieldAttributes.clear();

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
        parseAttributes(child, hide, show);
    }

    // Methods declared without a body are field_declarations too, the
    // function declarator inside any pointer or reference to what they
    // return (`Thing* find();`).
    TSNode declarator = field(node, "declarator");
    while (!ts_node_is_null(declarator) &&
           (type(declarator) == "pointer_declarator" ||
            type(declarator) == "reference_declarator"))
    {
      const TSNode inner = field(declarator, "declarator");
      declarator =
          ts_node_is_null(inner) && ts_node_named_child_count(declarator) > 0
              ? ts_node_named_child(
                    declarator, ts_node_named_child_count(declarator) - 1)
              : inner;
    }
    if (ts_node_is_null(declarator) ||
        type(declarator) == "function_declarator")
      return;

    if (hide || (!isPublic && !show))
      return;

    const TSNode typeNode = field(node, "type");
    base.type = normaliseType(text(typeNode));
    const std::string kindName = fieldKindOf(base.type, insideSfs());
    std::optional<std::string> kind;
    if (!kindName.empty())
      kind = kindName;
    if (kindName == "AssetRef" || kindName == "AssetRefList")
      base.assetType = *assetRefType(base.type, kindName);

    // The attributes take effect once the field's kind is known.
    if (kind)
      for (const FieldAttribute& attribute : m_fieldAttributes)
        applyAttribute(attribute, *kind, base);
    // Shared assets can't point at entities, and script fields can't yet.
    if (kind && isEntityKind(*kind) && model.kind != TypeKind::Component)
    {
      report(RuleId::EntityField,
             node,
             "Entity reference fields belong on components, so this " +
                 base.type +
                 " field is skipped. Move it to a component, which a system "
                 "can then read.");
      return;
    }

    // Every declarator: `float a, b;` declares two fields.
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);
      const char* fieldName = ts_node_field_name_for_named_child(node, i);
      if (!fieldName || std::string_view(fieldName) != "declarator")
        continue;

      if (type(child) != "field_identifier")
      {
        report(RuleId::FieldType,
               child,
               "The editor can't edit pointer, reference or array fields, so "
               "the field " +
                   text(child) +
                   " is skipped. Store a value it can edit instead, such as an "
                   "sfs::Entity or an asset reference.");
        continue;
      }
      if (!kind)
      {
        report(RuleId::FieldType,
               child,
               "The editor can't edit fields of type " + base.type +
                   " yet, so the field " + text(child) +
                   " is skipped. Use a type the editor can edit.");
        continue;
      }

      FieldModel fieldModel = base;
      fieldModel.name = text(child);
      fieldModel.nameRange = rangeOf(child);
      fieldModel.kind = *kind;
      model.fields.push_back(std::move(fieldModel));
    }
  }

  // Reads a field's [[sfs::...]] attributes: hide and show at once, the
  // rest into m_fieldAttributes for applyAttribute().
  void parseAttributes(TSNode declaration, bool& hide, bool& show)
  {
    const uint32_t count = ts_node_named_child_count(declaration);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode attribute = ts_node_named_child(declaration, i);
      const AttributeInfo* info = sfsAttribute(attribute);
      if (!info)
        continue;
      if (info->target != AttributeTarget::Field)
      {
        wrongTarget(*info, attribute);
        continue;
      }
      AttributeValue value;
      if (!attributeArguments(*info, attribute, value))
        continue;
      if (info->id == AttributeId::Hide)
        hide = true;
      else if (info->id == AttributeId::Show)
        show = true;
      else
        m_fieldAttributes.push_back({info, attribute, std::move(value)});
    }
  }

  // Sets an attribute's FieldModel member, unless it doesn't apply to fields
  // of `kind`.
  void applyAttribute(const FieldAttribute& attribute,
                      std::string_view kind,
                      FieldModel& fieldModel)
  {
    const AttributeInfo& info = *attribute.info;
    if (!appliesTo(info, kind))
    {
      report(RuleId::AttributeFieldType,
             attribute.node,
             "The attribute [[sfs::" + std::string(info.name) + "]] is for " +
                 attributeFieldTypes(info) +
                 " fields, so it's ignored here. Remove it, or change the "
                 "field's type.");
      return;
    }
    const AttributeValue& value = attribute.value;
    switch (info.id)
    {
    case AttributeId::Range:
      fieldModel.range = {value.first, value.second};
      break;
    case AttributeId::Color:
      fieldModel.color = true;
      break;
    case AttributeId::Script:
      fieldModel.script = true;
      break;
    case AttributeId::Texture:
      fieldModel.texture = true;
      break;
    case AttributeId::Materials:
      fieldModel.materials = true;
      break;
    case AttributeId::Material:
      fieldModel.material = true;
      break;
    case AttributeId::EmissionLight:
      fieldModel.emissionLight = true;
      break;
    case AttributeId::ShowIf:
      fieldModel.showIf = value.first;
      m_showIfs.push_back({attribute.node, value.first});
      break;
    case AttributeId::Part:
      fieldModel.part = value.first;
      break;
    case AttributeId::Hide:
    case AttributeId::Show:
    case AttributeId::Component:
      break; // read by parseAttributes, or not a field's
    }
  }

  // Warns about showIf attributes naming no reflected bool field of `model`.
  // A derived type's switch may be its base's field, so it isn't checked.
  void checkShowIfs(const TypeModel& model)
  {
    if (model.kind == TypeKind::Derived)
      return;
    for (const auto& [node, written] : m_showIfs)
    {
      const std::string_view target =
          std::string_view(written).substr(written.starts_with('!') ? 1 : 0);
      const bool found =
          std::any_of(model.fields.begin(),
                      model.fields.end(),
                      [&](const FieldModel& field)
                      { return field.name == target && field.kind == "Bool"; });
      if (!found)
        report(RuleId::ShowIfTarget,
               node,
               "The attribute [[sfs::showIf(" + written + ")]] names " +
                   std::string(target) + ", but " + model.name +
                   " has no reflected bool field by that name. Name one of " +
                   model.name + "'s bool fields.");
    }
  }

  // Whether the declarations being read are inside namespace sfs.
  bool insideSfs() const
  {
    return !m_namespaces.empty() && isSfsNamespace(m_namespaces.front());
  }

  std::string_view m_source;
  std::filesystem::path m_path;
  std::string m_target;
  std::vector<std::string> m_namespaces;
  ParseResult m_result;
  bool m_helper = false; // the file's class is a helper (no type, no warning)
  std::vector<FieldAttribute> m_fieldAttributes; // the field being read's
  // The showIf attributes of the type being read, with their argument.
  std::vector<std::pair<TSNode, std::string>> m_showIfs;
  int m_classDepth = 0; // class bodies entered
  int m_localDepth = 0; // templates and function bodies entered
  NestingLimit m_depth;
};

// Walks a UI header's syntax tree for its structs (see parseUiHeader).
class UiHeaderParser
{
public:
  UiHeaderParser(std::string_view source, const std::filesystem::path& path)
      : m_source(source), m_path(path)
  {
  }

  UiParseResult run()
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
    return std::move(m_result);
  }

private:
  std::string text(TSNode node) const
  {
    return std::string(nodeText(m_source, node));
  }

  static std::string type(TSNode node) { return ts_node_type(node); }

  static TSNode field(TSNode node, const char* name)
  {
    return childField(node, name);
  }

  void visit(TSNode node)
  {
    const std::string kind = type(node);
    if (kind == "namespace_definition")
    {
      const TSNode name = field(node, "name");
      const std::size_t depth = m_namespaces.size();
      if (!ts_node_is_null(name))
        m_namespaces.push_back(text(name));
      visitChildren(node);
      m_namespaces.resize(depth);
      return;
    }
    // Templates can't be made without arguments; nothing inside a function
    // is the header's.
    if (kind == "template_declaration" || kind == "function_definition")
      return;
    if ((kind == "class_specifier" || kind == "struct_specifier") &&
        !ts_node_is_null(field(node, "body")))
    {
      if (!ts_node_is_null(field(node, "name")))
        parseType(node, kind == "struct_specifier");
      return; // nested types are left out
    }
    visitChildren(node);
  }

  void visitChildren(TSNode node)
  {
    if (!m_depth.enter(m_path))
      return;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
      visit(ts_node_named_child(node, i));
    m_depth.leave();
  }

  void parseType(TSNode node, bool isStruct)
  {
    UiTypeModel model;
    model.name = text(field(node, "name"));
    model.header = m_path;
    for (const std::string& ns : m_namespaces)
      model.qualifiedName += ns + "::";
    model.qualifiedName += model.name;

    bool isPublic = isStruct;
    const TSNode body = field(node, "body");
    const uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode member = ts_node_named_child(body, i);
      const std::string memberKind = type(member);
      if (memberKind == "access_specifier")
        isPublic = text(member) == "public";
      else if (memberKind == "field_declaration" ||
               memberKind == "function_definition")
        parseMember(member, isPublic, model);
    }
    m_result.types.push_back(std::move(model));
  }

  // A data member's names, or a method (body() makes the type a view).
  void parseMember(TSNode node, bool isPublic, UiTypeModel& model)
  {
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);
      const std::string childKind = type(child);
      if (childKind == "storage_class_specifier" && text(child) == "static")
        return;
      if (childKind == "type_qualifier" && text(child) == "const")
        return; // can't be filled in
    }

    for (uint32_t i = 0; i < count; ++i)
    {
      const char* fieldName = ts_node_field_name_for_named_child(node, i);
      if (!fieldName || std::string_view(fieldName) != "declarator")
        continue;
      const TSNode declarator = ts_node_named_child(node, i);
      const std::string declaratorKind = type(declarator);
      if (declaratorKind == "function_declarator")
      {
        const TSNode name = field(declarator, "declarator");
        if (!ts_node_is_null(name) && text(name) == "body")
          model.view = true;
      }
      else if (declaratorKind == "field_identifier" && isPublic)
        model.fields.push_back(text(declarator));
    }
  }

  std::string_view m_source;
  std::filesystem::path m_path;
  std::vector<std::string> m_namespaces;
  UiParseResult m_result;
  NestingLimit m_depth;
};

} // namespace

std::string readFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

std::string_view nodeText(std::string_view source, TSNode node)
{
  if (ts_node_is_null(node))
    return {};
  const uint32_t start = ts_node_start_byte(node);
  return source.substr(start, ts_node_end_byte(node) - start);
}

TSNode childField(TSNode node, std::string_view name)
{
  return ts_node_child_by_field_name(
      node, name.data(), static_cast<uint32_t>(name.size()));
}

bool isSfsNamespace(std::string_view name)
{
  return name == "sfs" || name.starts_with("sfs::");
}

std::string fieldKindOf(std::string_view type, bool insideSfs)
{
  const std::string normal = normaliseType(type);
  // A reference to a scriptable object, sfs::AssetRef<T>; otherwise a fixed
  // type.
  if (assetRefType(normal, "AssetRef"))
    return "AssetRef";
  if (assetRefType(normal, "AssetRefList"))
    return "AssetRefList";
  if (const auto known = fieldKinds().find(normal); known != fieldKinds().end())
    return known->second;
  if (const auto inSfs = sfsFieldKinds().find(normal);
      inSfs != sfsFieldKinds().end() && insideSfs)
    return inSfs->second;
  return {};
}

SourceRange rangeOf(TSNode node)
{
  const TSPoint start = ts_node_start_point(node);
  const TSPoint end = ts_node_end_point(node);
  return {start.row, start.column, end.row, end.column};
}

ParseResult parseHeader(std::string_view source,
                        const std::filesystem::path& path,
                        const TSTree* tree)
{
  // UI code (Hud.ui.h: views, previews) is the game's own, never a script
  // or a component.
  if (path.filename().string().ends_with(".ui.h") || !tree)
    return {};
  return HeaderParser(source, path).run(tree);
}

ParseResult parseHeader(std::string_view source,
                        const std::filesystem::path& path)
{
  if (path.filename().string().ends_with(".ui.h"))
    return {};
  const std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(
      ts_parser_new(), &ts_parser_delete);
  ts_parser_set_language(parser.get(), tree_sitter_cpp());
  const std::unique_ptr<TSTree, decltype(&ts_tree_delete)> tree(
      ts_parser_parse_string(parser.get(),
                             nullptr,
                             source.data(),
                             static_cast<uint32_t>(source.size())),
      &ts_tree_delete);
  return parseHeader(source, path, tree.get());
}

UiParseResult parseUiHeader(std::string_view source,
                            const std::filesystem::path& path)
{
  return UiHeaderParser(source, path).run();
}

std::vector<Diagnostic> checkNames(const std::vector<TypeModel>& types)
{
  std::vector<Diagnostic> diagnostics;
  std::map<std::string_view, const TypeModel*, std::less<>> first;
  for (const TypeModel& type : types)
  {
    const auto [it, added] = first.emplace(type.name, &type);
    if (added)
      continue;
    const unsigned row = type.line > 0 ? type.line - 1 : 0;
    diagnostics.push_back({RuleId::DuplicateType,
                           type.header,
                           {row, 0, row, 0},
                           "More than one type is named " + type.name +
                               ", here and in " + it->second->header.string() +
                               ". Give each type a name of its own."});
  }
  return diagnostics;
}

} // namespace sfs::codegen
