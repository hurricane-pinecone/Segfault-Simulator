#pragma once

#include "sfs/codegen/model.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sfs::codegen
{

struct GeneratorOptions
{
  // Module name stored on every registered type (used to unregister a module
  // before it is unloaded).
  std::string module;

  // The registration function to define, fully qualified; it must already be
  // declared by an included header, e.g. "sfs::reflect::registerEngineTypes".
  std::string function;

  // Header that declares `function` (included first).
  std::string functionHeader = "engine/core/reflection/reflection.h";

  // Headers are included relative to this directory when they are inside it,
  // otherwise by absolute path.
  std::filesystem::path includeRoot;
};

/** C++ source registering `types` with the reflection registry. */
std::string generateRegistration(const std::vector<TypeModel>& types,
                                 const GeneratorOptions& options);

/**
 * C++ source for a UI preview module (the editor's UI Preview loads it):
 * each view in `types` as a preview (sfs::ui::Preview, named after it),
 * filled with placeholders (sfs::ui::Placeholder, specialised for every type
 * so lists of them are filled too). Defines an empty module entry point.
 */
std::string generateUiPreviews(const std::vector<UiTypeModel>& types,
                               const GeneratorOptions& options);

/**
 * The header of a target's event traits: an EventTraits specialization
 * (engine/core/events/eventTraits.h) for each of `events`, with its kind, a
 * request's responses and its event ancestors, after a declaration of each
 * type. Every file of the target includes it first.
 *
 * @param events the target's event types
 * @param plainTypes the qualified names of its other namespace-scope types,
 *        so a request listing one is told so
 * @param diagnostics where problems with the declarations go: a request
 *        listing a type that isn't a response, an event deriving from one of
 *        another kind, a request or response in a family
 * @return the header's text
 */
std::string generateEventTraits(const std::vector<EventModel>& events,
                                const std::vector<std::string>& plainTypes,
                                std::vector<Diagnostic>& diagnostics);

/**
 * The part of a target's traits header (after generateEventTraits()'s text)
 * for its scene data types: a SceneDataTraits specialization
 * (engine/core/ecs/sceneData.h) for each type marked [[sfs::scene_data]],
 * after a declaration of each type.
 * Each type is written once, however often it's seen.
 *
 * @param types the target's types; those that aren't scene data are skipped
 * @return the text; empty when none is scene data
 */
std::string generateSceneDataTraits(const std::vector<TypeModel>& types);

/**
 * The qualified names a type name written inside some namespaces may mean,
 * innermost first, as C++ looks a name up through the enclosing namespaces.
 *
 * @param written the name as written, e.g. "nav::PathFound"; a leading "::"
 *        names it from the global namespace
 * @param scopes the enclosing namespaces, outermost first ("a::b" for a
 *        nested one, read as `a` then `b`); "" for an unnamed one, which
 *        adds nothing
 * @return e.g. "game::nav::PathFound", then "nav::PathFound"
 */
std::vector<std::string>
lookupCandidates(std::string_view written,
                 const std::vector<std::string>& scopes);

/** The names a type spelling may mean where it's written (lookUpName()). */
struct NameLookup
{
  /** The qualified names it may mean, the likeliest first. */
  std::vector<std::string> candidates;
  /** Whether a using-declaration or an alias was followed to get them. */
  bool followed = false;
  /** Whether a using-directive the spelling sees added some of them. */
  bool directed = false;
};

/**
 * The qualified names a type spelling written at a place may mean, as C++
 * looks it up through the names the code there sees:
 * - a using-declaration or type alias whose name is the whole spelling, or
 *   one in a namespace the spelling qualifies (`z::Found` through
 *   `namespace z { using a::Found; }`), and a namespace alias naming its
 *   first part, are followed, each looked up from where it's written (up to
 *   eight deep);
 * - then the names the namespaces it's in give (lookupCandidates());
 * - then those each using-directive it sees adds.
 * A using name is seen when it's written before the spelling, in a
 * namespace the spelling is in, and, inside a block, only to the block's
 * end.
 *
 * @param spelling the type as written, normalised
 * @param scopes the namespaces it's written in, outermost first
 * @param offset its byte in the file
 * @param usings the file's using names
 * @return the names, and how they were found
 */
NameLookup lookUpName(std::string_view spelling,
                      const std::vector<std::string>& scopes,
                      uint32_t offset,
                      std::span<const UsingName> usings);

/**
 * The C++ type a field kind stands for, as the generated code spells it.
 *
 * @param kind a FieldModel::kind, e.g. "Vec3"
 * @return e.g. "glm::vec3"; empty for an asset reference or an unknown kind
 */
std::string_view cppTypeOfKind(std::string_view kind);

/** "castsShadow" -> "Casts Shadow", "MeshRendererComponent" -> "Mesh Renderer". */
std::string displayName(std::string_view identifier, bool stripComponentSuffix);

} // namespace sfs::codegen
