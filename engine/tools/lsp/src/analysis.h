#pragma once

#include "document.h"

#include "lints.h"

#include <sfs/codegen/rules.h>

struct TSParser;

namespace sfs::lsp
{

class ProjectIndex;

/** Files larger than this get no parse and no diagnostics. */
inline constexpr std::size_t kMaxAnalysedSize = 2u * 1024u * 1024u;

/**
 * Works out a document's diagnostics: codegen's for a header codegen reads
 * (from the document's own tree), the lints, then what its sfs-ignore comments
 * and `disabled` hide. Reparses the tree first if it's stale. Fills
 * `document.diagnostics`, `suppressions` and `role`, and clears `dirty`.
 *
 * @param document the document
 * @param parser the server's C++ parser
 * @param linter the server's linter
 * @param index the project's types
 * @param disabled rules the user turned off (errors and bad-ignore stay on)
 * @param parsed for a header codegen reads, gains the types codegen found
 *   (for the project index); may be null
 * @param read gives other files' text to lints that read them; null: from
 *   disk
 */
void analyse(Document& document,
             TSParser* parser,
             Linter& linter,
             const ProjectIndex& index,
             const codegen::RuleSet& disabled,
             codegen::ParseResult* parsed = nullptr,
             const FileReader* read = nullptr);

/**
 * Whether codegen reads a document: a script header of the project, or another
 * header with an [[sfs::...]] attribute in its code (not in a comment).
 */
bool isCodegenHeader(const Document& document, const ProjectIndex& index);

} // namespace sfs::lsp
