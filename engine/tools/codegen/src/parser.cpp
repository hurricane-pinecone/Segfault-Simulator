#include "sfs/codegen/parser.h"

#include "sfs/codegen/attributes.h"
#include "sfs/codegen/model.h"
#include "sfs/codegen/rules.h"
#include "sfs/codegen/suppressions.h"

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-cpp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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
      {"sfs::Entity", "Entity"},
      {"std::vector<sfs::Entity>", "EntityList"},
      {"std::vector<sfs::ShaderParameter>", "ShaderParameters"},
  };
  return kinds;
}

// Spellings of the kinds that name an sfs type unqualified only inside
// namespace sfs (elsewhere `Entity` may be anything).
const std::map<std::string, std::string, std::less<>>& sfsFieldKinds()
{
  static const std::map<std::string, std::string, std::less<>> kinds = {
      {"Entity", "Entity"},
      {"std::vector<Entity>", "EntityList"},
      {"std::vector<ShaderParameter>", "ShaderParameters"},
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
  constexpr std::string_view OPEN = "<";

  if (!type.starts_with(OPEN) || !type.ends_with('>'))
    return std::nullopt;

  std::string inner(type.substr(OPEN.size(), type.size() - OPEN.size() - 1));

  while (!inner.empty() && inner.front() == ' ')
    inner.erase(inner.begin());

  while (!inner.empty() && inner.back() == ' ')
    inner.pop_back();

  return inner.empty() ? std::nullopt : std::optional(inner);
}

} // namespace

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

void readUsingName(TSNode node,
                   std::string_view source,
                   const std::filesystem::path& file,
                   std::vector<std::string> scopes,
                   std::vector<UsingName>& out)
{
  const std::string_view kind =
      ts_node_is_null(node) ? std::string_view{} : ts_node_type(node);
  UsingName found{
      file, {}, {}, false, std::move(scopes), ts_node_start_byte(node)};

  if (kind == "alias_declaration" || kind == "type_definition")
  {
    found.name = nodeText(
        source,
        childField(node, kind == "alias_declaration" ? "name" : "declarator"));
    found.target = normaliseType(nodeText(source, childField(node, "type")));
  }
  else if (kind == "namespace_alias_definition")
  {
    const uint32_t count = ts_node_named_child_count(node);

    if (count < 2)
      return;

    found.name = nodeText(source, childField(node, "name"));
    found.target =
        normaliseType(nodeText(source, ts_node_named_child(node, count - 1)));
    found.isNamespace = true;
  }
  else if (kind == "using_declaration")
  {
    if (ts_node_named_child_count(node) != 1)
      return;

    found.target =
        normaliseType(nodeText(source, ts_node_named_child(node, 0)));
    found.isNamespace =
        normaliseType(nodeText(source, node)).starts_with("using namespace");

    if (!found.isNamespace)
    {
      const std::size_t colons = found.target.rfind("::");
      found.name = colons == std::string::npos
                       ? found.target
                       : found.target.substr(colons + 2);
    }
  }
  else
    return;

  if (found.target.empty() || (found.name.empty() && !found.isNamespace))
    return;

  // In a block (a function body), it's seen only to the block's end.
  for (TSNode at = ts_node_parent(node); !ts_node_is_null(at);
       at = ts_node_parent(at))
    if (std::string_view(ts_node_type(at)) == "compound_statement")
    {
      found.end = ts_node_end_byte(at);
      break;
    }

  out.push_back(std::move(found));
}

namespace
{

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
  // Roles: a request's response types, as written; empty when not named.
  std::string success;
  std::string progress;
  std::string failure;
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

    if (!m_result.type && !m_helper && !m_events && m_result.components.empty())
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

      m_scopes.push_back(ts_node_is_null(name) ? std::string() : text(name));
      const bool inlined = isInlineNamespace(node);
      m_inlineDepth += inlined ? 1 : 0;
      visitChildren(node);
      m_inlineDepth -= inlined ? 1 : 0;
      m_namespaces.resize(depth);
      m_scopes.pop_back();
      return;
    }

    // tree-sitter's grammar can't read an attribute on an enum, so only a
    // union is checked.
    if (kind == "union_specifier")
    {
      if (typeMarks(node).events != 0)
        report(RuleId::EventDeclaration,
               node,
               "An event attribute goes on a struct, not a union. Make the "
               "event a struct.");

      visitChildren(node);
      return;
    }

    if ((kind == "class_specifier" || kind == "struct_specifier") &&
        !ts_node_is_null(field(node, "body")))
    {
      const TSNode name = field(node, "name");
      const TypeMarks marks = typeMarks(node);
      const bool marked = marks.component || marks.sceneData;

      if (marks.events != 0)
      {
        // An event is sent and read, never registered: no component, no
        // reflection, and it may be named after its header.
        m_events = true;

        if (marks.component)
          report(RuleId::EventAndComponent,
                 node,
                 "The event " + text(name) +
                     " can't be a component too, since a component is state "
                     "and an event is what one system tells another. Remove "
                     "[[sfs::component]] or the event attribute.");

        if (marks.sceneData)
          report(RuleId::SceneDataDeclaration,
                 node,
                 "The event " + text(name) +
                     " can't be [[sfs::scene_data]] too, since scene data is "
                     "state the scene holds and an event is what one system "
                     "tells another. Remove [[sfs::scene_data]] or the event "
                     "attribute.");

        parseEvent(node, marks);
      }
      else if (m_classDepth == 0 && !ts_node_is_null(name) &&
               lowercase(text(name)) == m_target)
      {
        if (m_result.type)
          report(RuleId::DuplicateType,
                 node,
                 "More than one type is named " + text(name) +
                     ". Give each type a name of its own.");
        else
          parseType(node, kind == "struct_specifier", marks);
      }
      else if (marks.sceneData && ts_node_is_null(name))
        report(RuleId::SceneDataDeclaration,
               node,
               "An unnamed type can't be [[sfs::scene_data]], since "
               "sceneData<T>() names it. Give it a name, or remove "
               "[[sfs::scene_data]].");
      else if (marks.sceneData && (m_classDepth > 0 || m_localDepth > 0))
        report(RuleId::SceneDataDeclaration,
               node,
               "The scene data type " + text(name) +
                   " is nested or a template, so it isn't registered. "
                   "Declare it as a plain type at namespace scope.");
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
        parseType(node, kind == "struct_specifier", marks);

      if (marks.events == 0 && !ts_node_is_null(name) && m_classDepth == 0 &&
          m_localDepth == 0)
        m_result.plainTypes.push_back(qualified(text(name)));

      // Nested types are never registered, but a marked one is warned about.
      ++m_classDepth;
      visitChildren(field(node, "body"));
      --m_classDepth;
      return;
    }

    if (kind == "using_declaration" || kind == "alias_declaration" ||
        kind == "type_definition" || kind == "namespace_alias_definition")
      readUsingName(node, m_source, m_path, m_scopes, m_usings);

    if (kind == "using_declaration")
    {
      const std::string written = normaliseType(text(node));

      if (written.starts_with("using namespace sfs;"))
        m_usingSfs = true;
      else if (written.starts_with("using sfs::"))
        m_usingSfsNames.push_back(written.substr(11, written.size() - 12));
    }
    // `using X = ...;` and `typedef ... X;` alike.
    else if (kind == "alias_declaration" || kind == "type_definition")
    {
      const TSNode name =
          field(node, kind == "alias_declaration" ? "name" : "declarator");

      if (!ts_node_is_null(name) &&
          holdsEventHandle(normaliseType(text(field(node, "type")))))
        m_handleAliases.push_back(text(name));
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

  // The type attributes a class specifier carries.
  struct TypeMarks
  {
    bool component = false; // [[sfs::component]]
    bool sceneData = false; // [[sfs::scene_data]]
    int events = 0;         // event attributes ([[sfs::event_news]], ...)
    EventKind event = EventKind::News; // the first one's kind
    // [[sfs::event_request(S, progress = P, failure = F)]]'s roles.
    std::string success;
    std::string progress;
    std::string failure;
  };

  // Which of [[sfs::component]], [[sfs::scene_data]] and the event attributes
  // a class specifier carries; warns about any other [[sfs::...]] attribute
  // on it.
  TypeMarks typeMarks(TSNode node)
  {
    TypeMarks marks;
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

        if (!attributeArguments(*info, attribute, value))
          continue;

        if (info->id == AttributeId::Component)
        {
          marks.component = true;
          continue;
        }

        if (info->id == AttributeId::SceneData)
        {
          marks.sceneData = true;
          continue;
        }

        EventKind event = EventKind::News;

        if (info->id == AttributeId::EventEntityNews)
          event = EventKind::EntityNews;
        else if (info->id == AttributeId::EventRequest)
          event = EventKind::Request;
        else if (info->id == AttributeId::EventResponse)
          event = EventKind::Response;
        else if (info->id != AttributeId::EventNews)
          continue;

        if (++marks.events == 2)
          report(RuleId::EventDeclaration,
                 attribute,
                 "A type takes one event attribute, so this one is a second. "
                 "Keep the one that says what the event is.");

        if (marks.events == 1)
        {
          marks.event = event;
          marks.success = std::move(value.success);
          marks.progress = std::move(value.progress);
          marks.failure = std::move(value.failure);
        }
      }
    }

    return marks;
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
    if (isEventAttribute(info.id))
    {
      report(RuleId::EventDeclaration,
             attribute,
             "The attribute [[sfs::" + std::string(info.name) +
                 "]] goes on a struct, not a field. Move it to the struct.");
      return;
    }

    const bool onType = info.target == AttributeTarget::Type;
    report(RuleId::AttributeTarget,
           attribute,
           "The attribute [[sfs::" + std::string(info.name) + "]] goes on " +
               (onType ? "a type, not a field" : "a field, not a type") +
               ", so it's ignored here. Move it to " +
               (onType ? "the type." : "a field."));
  }

  // Whether a node names a type: `A`, `ns::A`.
  static bool isTypeName(TSNode node)
  {
    const std::string kind = type(node);
    return kind == "identifier" || kind == "qualified_identifier" ||
           kind == "type_identifier";
  }

  // A request's roles, `(Success, progress = P, failure = F)`, into `value`;
  // false, with an event-declaration error, when they're malformed.
  bool readRoles(const AttributeInfo& info,
                 TSNode attribute,
                 TSNode arguments,
                 AttributeValue& value)
  {
    const std::string form = "Write [[sfs::" + std::string(info.name) +
                             std::string(info.arguments) + "]].";
    const auto fail = [&](TSNode at, std::string message)
    {
      report(RuleId::EventDeclaration, at, message + " " + form);
      return false;
    };
    const uint32_t count =
        ts_node_is_null(arguments) ? 0 : ts_node_named_child_count(arguments);

    if (count == 0)
      return fail(attribute, "A request names its success response first.");

    // A stray comma, colon or missing type leaves the list broken: say so
    // rather than describe the pieces the parser recovered.
    if (ts_node_has_error(arguments))
      return fail(arguments,
                  "A request's arguments are its success response type, then "
                  "roles written role = Type, but " +
                      text(arguments) + " isn't that.");

    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode argument = ts_node_named_child(arguments, i);

      if (isTypeName(argument))
      {
        if (i > 0)
          return fail(argument,
                      "A request has one success response, named first; " +
                          text(argument) +
                          " needs a role (progress = or failure =).");

        value.success = normaliseType(text(argument));
        continue;
      }

      const TSNode left = field(argument, "left");
      const TSNode right = field(argument, "right");

      if (type(argument) != "assignment_expression" ||
          type(left) != "identifier" || !isTypeName(right) ||
          text(field(argument, "operator")) != "=")
        return fail(argument,
                    "A request's arguments are its success response type, "
                    "then roles written role = Type, but " +
                        text(argument) + " is neither.");

      const std::string role = text(left);

      if (role == "success")
        return fail(argument,
                    "A request's success response is written bare, first: (" +
                        text(right) + ", ...), not " + text(argument) + ".");

      if (i == 0)
        return fail(argument,
                    "A request names its success response first, before " +
                        text(argument) + ".");

      std::string* slot = role == "progress"  ? &value.progress
                          : role == "failure" ? &value.failure
                                              : nullptr;

      if (!slot)
      {
        std::string message = "A request has no role " + role +
                              "; its roles are progress and failure.";

        // "fail" is the server's word for the failure role.
        for (const std::string_view known : {"progress", "failure"})
          if (editDistance(role, known) <= 2 ||
              (role.size() >= 4 && known.starts_with(role)))
            message +=
                " Use " + std::string(known) + " if that's what you meant.";

        return fail(left, message);
      }

      if (!slot->empty())
        return fail(argument,
                    "The request names its " + role +
                        " response twice. Name each role once.");

      *slot = normaliseType(text(right));
    }

    return true;
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
    case AttributeArgs::Roles:
      return readRoles(info, attribute, arguments, value);
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
  // header's [[sfs::component]] or [[sfs::scene_data]] types.
  void parseType(TSNode node, bool isStruct, const TypeMarks& marks)
  {
    const bool marked = marks.component || marks.sceneData;
    TypeModel model;
    model.name = text(field(node, "name"));
    model.nameRange = rangeOf(field(node, "name"));
    model.header = m_path;
    model.line = ts_node_start_point(node).row + 1;

    for (const std::string& ns : m_namespaces)
      model.qualifiedName += ns + "::";

    model.qualifiedName += model.name;
    model.scopes = m_scopes;
    model.classKey = isStruct ? "struct" : "class";
    model.kind = classify(node, model);

    const bool fileType = lowercase(model.name) == m_target;

    if (marks.sceneData)
    {
      if (marks.component)
      {
        report(RuleId::SceneDataDeclaration,
               node,
               model.name + " can't be both [[sfs::scene_data]] and "
                            "[[sfs::component]]: scene data is held by the "
                            "scene, a component by an entity. Remove one.");
        return;
      }

      if (model.kind != TypeKind::Component)
      {
        report(RuleId::SceneDataDeclaration,
               node,
               model.name + " derives from " + baseOf(node) +
                   ", but scene data is a plain type with no base. Give " +
                   model.name +
                   " the fields itself, or remove "
                   "[[sfs::scene_data]].");
        return;
      }

      model.kind = TypeKind::SceneData;
    }
    else if (marked && model.kind != TypeKind::Component)
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

  // A class specifier's base types, as written, and which are virtual.
  std::vector<EventBase> basesOf(TSNode node) const
  {
    std::vector<EventBase> bases;
    const uint32_t count = ts_node_named_child_count(node);

    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode clause = ts_node_named_child(node, i);

      if (type(clause) != "base_class_clause")
        continue;

      // `virtual` is an unnamed token before its base, up to the next comma.
      bool isVirtual = false;
      const uint32_t children = ts_node_child_count(clause);

      for (uint32_t c = 0; c < children; ++c)
      {
        const TSNode base = ts_node_child(clause, c);
        const std::string kind = type(base);

        if (kind == "virtual")
          isVirtual = true;
        else if (kind == ",")
          isVirtual = false;
        else if (ts_node_is_named(base) && kind != "access_specifier")
          bases.push_back({normaliseType(text(base)), isVirtual});
      }
    }

    return bases;
  }

  // Its first base type, as written; empty with none.
  std::string baseOf(TSNode node) const
  {
    std::vector<EventBase> bases = basesOf(node);
    return bases.empty() ? std::string() : std::move(bases.front().name);
  }

  // `name` with the enclosing named namespaces.
  std::string qualified(const std::string& name) const
  {
    std::string out;

    for (const std::string& ns : m_namespaces)
      out += ns + "::";

    return out + name;
  }

  // Whether a namespace definition is inline: `inline namespace v1`, or an
  // inline part of a nested one, `namespace a::inline v1`.
  bool isInlineNamespace(TSNode node) const
  {
    const TSNode first = ts_node_child(node, 0);

    if (!ts_node_is_null(first) && type(first) == "inline")
      return true;

    const TSNode name = field(node, "name");
    return !ts_node_is_null(name) &&
           type(name) == "nested_namespace_specifier" &&
           text(name).find("inline ") != std::string::npos;
  }

  // Records an event type, which must be a named struct at namespace scope.
  void parseEvent(TSNode node, const TypeMarks& marks)
  {
    const TSNode name = field(node, "name");

    if (ts_node_is_null(name) || m_classDepth > 0 || m_localDepth > 0)
    {
      report(RuleId::EventDeclaration,
             node,
             "An event is a named struct at namespace scope, not nested in a "
             "class, a function or a template, so the compiler can be told "
             "what it is. Move it out and name it.");
      return;
    }

    // The traits header declares each event before naming it, and a
    // declaration outside the inline namespace would be another type.
    if (m_inlineDepth > 0)
    {
      report(RuleId::EventDeclaration,
             name,
             "The event " + text(name) +
                 " is in an inline namespace, which its traits can't name. "
                 "Declare it in a plain namespace.");
      return;
    }

    EventModel event;
    event.name = text(name);
    event.qualifiedName = qualified(event.name);
    event.scopes = m_scopes;
    event.kind = marks.event;
    event.classKey = type(node) == "class_specifier" ? "class" : "struct";
    event.success = marks.success;
    event.progress = marks.progress;
    event.failure = marks.failure;
    event.bases = basesOf(node);
    event.header = m_path;
    event.nameRange = rangeOf(name);
    event.offset = ts_node_start_byte(name);

    // Those written before it; lookUpName() tells which it sees.
    for (const UsingName& using_ : m_usings)
      if (using_.offset < event.offset)
        event.usings.push_back(using_);

    m_result.events.push_back(std::move(event));
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
        else if (name == "Service" || name == "sfs::Service")
          kind = TypeKind::Service;
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
                        "sfs::Service, sfs::MeshSource, "
                        "sfs::ScriptableObject or another reflected type, or "
                        "from nothing for a component.");
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

    // A system's or object's event handles are kept as members on purpose:
    // they're never edited, and not worth a warning.
    if (holdsEventHandle(base.type))
      return;

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
    case AttributeId::SceneData:
    case AttributeId::EventNews:
    case AttributeId::EventEntityNews:
    case AttributeId::EventRequest:
    case AttributeId::EventResponse:
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

  // Whether a field type holds one of SFS's event handles, perhaps in a
  // container: `sfs::EventConsumer<E>`, `std::vector<sfs::Responder<R>>`,
  // unqualified where SFS's names are visible, or an alias this header
  // declares for one.
  bool holdsEventHandle(std::string_view type) const
  {
    // Whether the name at `at` stands alone: not part of a longer name, and
    // either qualified by sfs:: or, unqualified, where it's visible.
    const auto alone = [&](std::size_t at, std::size_t size, bool visible)
    {
      const auto part = [](char c)
      { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

      if (at + size < type.size() && part(type[at + size]))
        return false;

      const std::string_view before = type.substr(0, at);

      if (before.ends_with("sfs::"))
        return before.size() == 5 || !part(before[before.size() - 6]);

      return visible &&
             (before.empty() || (!part(before.back()) && before.back() != ':'));
    };

    for (const std::string_view handle : kEventHandleTypes)
    {
      const bool visible =
          insideSfs() || m_usingSfs ||
          std::find(m_usingSfsNames.begin(), m_usingSfsNames.end(), handle) !=
              m_usingSfsNames.end();

      for (std::size_t at = type.find(handle); at != std::string_view::npos;
           at = type.find(handle, at + 1))
        if (alone(at, handle.size(), visible))
          return true;
    }

    for (const std::string& alias : m_handleAliases)
      for (std::size_t at = type.find(alias); at != std::string_view::npos;
           at = type.find(alias, at + 1))
        if (alone(at, alias.size(), true))
          return true;

    return false;
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
  bool m_helper = false;   // the file's class is a helper (no type, no warning)
  bool m_events = false;   // the header declares event types
  int m_inlineDepth = 0;   // how many inline namespaces the walk is in
  bool m_usingSfs = false; // the header says `using namespace sfs;`
  std::vector<std::string> m_handleAliases; // its aliases of handle types
  std::vector<std::string> m_usingSfsNames; // `using sfs::X;`'s X
  std::vector<UsingName> m_usings; // every using name, in the order written
  std::vector<FieldAttribute> m_fieldAttributes; // the field being read's
  // The showIf attributes of the type being read, with their argument.
  std::vector<std::pair<TSNode, std::string>> m_showIfs;
  // The enclosing namespaces, "" for an unnamed one: where events live.
  std::vector<std::string> m_scopes;
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
