#pragma once

#include "engine/core/util/typeList.h"

#include <cstdint>

// What the compiler knows about each event type: its kind, a request's
// responses and an event's event ancestors. sfs-codegen reads the attributes
// (`[[sfs::event_news]]`, `[[sfs::event_entity_news]]`,
// `[[sfs::event_request(Success, progress = P, failure = F)]]`,
// `[[sfs::event_response]]`) and
// writes an EventTraits specialization for each marked type into a header
// every file of the type's target includes first (docs/core/events, and
// engine/docs/core/events for the header). Every call that sends or receives
// a type checks it against these at compile time.
namespace sfs::detail
{

/** What kind of event a type is. */
enum class EventKind : uint8_t
{
  /** Not marked as an event. */
  None,
  /** `[[sfs::event_news]]`: news, sent with send(event). */
  News,
  /**
   * `[[sfs::event_entity_news]]`: news about one entity, sent with
   * send(entity, event). Rows give the entity, and each send supersedes the
   * entity's earlier one for readLatest().
   */
  EntityNews,
  /**
   * `[[sfs::event_request(Success, progress = P, failure = F)]]`: about one
   * entity, asked with a requester and answered with its success response,
   * or its failure, after any number of progress responses (both
   * optional).
   */
  Request,
  /**
   * `[[sfs::event_response]]`: what a request gets back, sent only through a
   * Responder. Its rows give the request's entity.
   */
  Response,
};

/**
 * What a type is as an event. This is a type that isn't one; sfs-codegen
 * specialises it for each marked type.
 */
template <typename T>
struct EventTraits
{
  /** Its kind. */
  static constexpr EventKind kind = EventKind::None;
  /** A request's success response: its final answer. */
  using Success = void;
  /** A request's progress response, partial answers; void when undeclared. */
  using Progress = void;
  /** A request's failure response, a final answer; void when undeclared. */
  using Failure = void;
  /** Its event base types, nearest first, every level. */
  using Ancestors = TypeList<>;
};

/**
 * The words for a kind, for logs and the debug view: "news", "entity news",
 * "request", "response".
 * @param kind the kind
 * @return the words
 */
const char* eventKindName(EventKind kind);

} // namespace sfs::detail

// The engine's own event types' traits, made by sfs-codegen.
#if __has_include("engine/generated/engineEventTraits.gen.h")
  #include "engine/generated/engineEventTraits.gen.h" // IWYU pragma: export
#endif
