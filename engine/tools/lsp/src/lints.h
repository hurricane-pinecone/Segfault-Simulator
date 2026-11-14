#pragma once

#include <sfs/codegen/model.h>

#include <tree_sitter/api.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sfs::lsp
{

class ProjectIndex;

/**
 * Gives another file's text, for a lint that reads a class's header: an open
 * buffer's, else the file on disk; empty when it can't be read.
 */
using FileReader = std::function<std::string(const std::filesystem::path&)>;

/**
 * sfs-lsp's lints of the SFS API and architecture rules: query terms, system
 * rules, attributes outside codegen and the entity API. One cursor walk of a
 * file's tree that dispatches on node kinds; ERROR subtrees are skipped, so a
 * misparse means a missed finding, never a false one. The entity API rules
 * then look around the sites the walk found. Holds only scratch reused from
 * file to file.
 */
class Linter
{
public:
  Linter();
  ~Linter();

  /**
   * Lints one file.
   *
   * @param source the file's text
   * @param tree its syntax tree
   * @param path the file, for the diagnostics
   * @param index the project's types, to tell its systems
   * @param out gains a diagnostic for each finding, with its fix if it has one
   * @param read gives other files' text (a class's header); null: from disk
   */
  void run(std::string_view source,
           const TSTree* tree,
           const std::filesystem::path& path,
           const ProjectIndex& index,
           std::vector<codegen::Diagnostic>& out,
           const FileReader* read = nullptr);

  /** The node kinds the walk tells apart. */
  enum class Kind : uint8_t
  {
    Other,
    Error,
    NamespaceDefinition,
    ClassSpecifier,
    FunctionDefinition,
    LambdaExpression,
    CallExpression,
    TemplateFunction,
    TemplateMethod,
    TemplateArgumentList,
    TemplateType,
    TypeDescriptor,
    QualifiedIdentifier,
    UsingDeclaration,
    FieldDeclaration,
    BaseClassClause,
    InitDeclarator,
    ReferenceDeclarator,
    PointerDeclarator,
    PointerExpression,
    AssignmentExpression,
    FieldExpression,
    ExpressionStatement,
    ParameterPackExpansion,
    AttributeDeclaration,
    Attribute,
    FunctionDeclarator,
    StorageClassSpecifier,
    ArgumentList,
    Identifier,
    ParameterDeclaration,
    ForRangeLoop,
    Declaration,
    This,
    AliasDeclaration,
    IfStatement,
    WhileStatement,
    ForStatement,
    ConditionClause,
    InitStatement,
    CompoundStatement,
    BinaryExpression,
    UnaryExpression,
    ConditionalExpression,
    ParenthesizedExpression,
    ReturnStatement,
    BreakStatement,
    ContinueStatement,
    FieldDeclarationList,
    TemplateDeclaration,
    AccessSpecifier,
    Comment,
    StructuredBindingDeclarator,
    LambdaCaptureSpecifier,
    LambdaCaptureInitializer,
    LambdaDefaultCapture,
    SubscriptExpression,
    FieldInitializerList,
    FieldIdentifier,
    CatchClause,
    UpdateExpression,
    TryStatement,
    DeleteMethodClause,
    DefaultMethodClause,
    OptionalParameterDeclaration,
    TranslationUnit,
    SwitchStatement,
    DestructorName,
  };

  /** The kind of `node`. */
  Kind kindOf(TSNode node) const;

private:
  struct Walk;
  std::vector<Kind> m_kinds;    // by tree-sitter symbol
  std::unique_ptr<Walk> m_walk; // its containers are kept from run to run
};

} // namespace sfs::lsp
