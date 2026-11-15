#pragma once

#include "engine/core/events/eventTraits.h"

#include <cstddef>
#include <type_traits>

// The compile-time rules every call that sends or receives an event type
// checks it against, read from its traits (eventTraits.h; docs/core/events).
// A broken rule fails to compile with a message saying how to fix it, and
// names the types in the failing template's arguments.
namespace sfs::detail
{

/** How many times A is in a list of types. */
template <typename A, typename... TList>
inline constexpr std::size_t kCount =
    (std::size_t{0} + ... + (std::is_same_v<A, TList> ? 1 : 0));

/** How many types a TypeList has. */
template <typename TList>
inline constexpr std::size_t kListSize = 0;
template <typename... TTypes>
inline constexpr std::size_t kListSize<TypeList<TTypes...>> = sizeof...(TTypes);

/** Whether A is in a TypeList. */
template <typename A, typename TList>
inline constexpr bool kListed = false;
template <typename A, typename... TTypes>
inline constexpr bool kListed<A, TypeList<TTypes...>> =
    kCount<A, TTypes...> != 0;

/**
 * Responses of type A to requests of type R, as their queue holds them: an A
 * (a row gives it as one) that knows the request it responds to, so each
 * request's responses have a queue of their own, keyed and filtered by the
 * request type's channel.
 */
template <typename TRequest, typename TResponse>
struct ResponseTo : TResponse
{
  /** The request type it responds to. */
  using Request = TRequest;
  /** The response type. */
  using Value = TResponse;
};

/** Whether T is a response queue's type, a ResponseTo<R, A>. */
template <typename T>
inline constexpr bool kResponseQueue = false;
template <typename TRequest, typename TResponse>
inline constexpr bool kResponseQueue<ResponseTo<TRequest, TResponse>> = true;

/** Whether T is a response queue's type, a ResponseTo<R, A>. */
template <typename T>
concept ResponseQueueType = kResponseQueue<T>;

/** T's kind as an event. */
template <typename T>
inline constexpr EventKind kKind = EventTraits<T>::kind;

/**
 * Whether a type's events are about an entity, which its rows give: entity
 * news, requests and responses (response queues included).
 */
template <typename T>
inline constexpr bool kKeyed =
    ResponseQueueType<T> || kKind<T> == EventKind::EntityNews ||
    kKind<T> == EventKind::Request || kKind<T> == EventKind::Response;

/**
 * Whether a keyed type's queue has a channel of its own: each send starts
 * the key's next generation, superseding the last (entity news, requests).
 */
template <typename T>
inline constexpr bool kOwnChannel =
    kKind<T> == EventKind::EntityNews || kKind<T> == EventKind::Request;

/** Whether a type is a request that gets responders. */
template <typename T>
inline constexpr bool kRequest = kKind<T> == EventKind::Request;

/** Whether a type is a response, sent only through a Responder. */
template <typename T>
inline constexpr bool kResponse = kKind<T> == EventKind::Response;

/** Fails to compile, naming a type used as an event that isn't marked. */
template <typename T>
struct NotAnEvent
{
  static_assert(sizeof(T) == 0,
                "This type (NotAnEvent<Type>) isn't an event: mark it "
                "[[sfs::event_news]], or [[sfs::event_entity_news]], "
                "[[sfs::event_request(...)]] or [[sfs::event_response]]");
};

/** Fails to compile, naming a response type that's sent another way. */
template <typename T>
struct IsAResponse
{
  static_assert(sizeof(T) == 0,
                "This type (IsAResponse<Type>) is a response: send it with "
                "responder.respond()");
};

/** Fails to compile, naming a request's listed type that isn't a response. */
template <typename TRequest, typename TResponse>
struct NotAResponse
{
  static_assert(sizeof(TResponse) == 0,
                "A request lists only response types (NotAResponse<Request, "
                "Type>): mark it [[sfs::event_response]]");
};

/** Fails to compile, naming an event whose event base is another kind. */
template <typename TEvent, typename TBase>
struct AncestorOfAnotherKind
{
  static_assert(sizeof(TBase) == 0,
                "An event (AncestorOfAnotherKind<Event, Base>) is the same "
                "kind as its event bases: [[sfs::event_news]] under "
                "[[sfs::event_news]], [[sfs::event_entity_news]] under "
                "[[sfs::event_entity_news]]; requests and responses take no "
                "part");
};

// One of a request's response types.
template <typename TRequest, typename TResponse>
consteval bool checkResponseType()
{
  if constexpr (!kResponse<TResponse>)
    static_cast<void>(sizeof(NotAResponse<TRequest, TResponse>));
  else
    static_assert(std::is_class_v<TResponse> && !std::is_final_v<TResponse>,
                  "A response type is a struct, not final");
  return true;
}

// A request's responses: at least one, each once, each a response.
template <typename TRequest, typename... TResponses>
consteval bool checkResponses(TypeList<TResponses...>*)
{
  static_assert(sizeof...(TResponses) != 0,
                "A request lists at least one response type: "
                "[[sfs::event_request(Response, ...)]]");
  static_assert(((kCount<TResponses, TResponses...> == 1) && ...),
                "A request lists each response type once");
  return (checkResponseType<TRequest, TResponses>() && ...);
}

// An event's ancestors: bases of it, of its own kind, never requests or
// responses.
template <typename TEvent, typename... TBases>
consteval bool checkAncestors(TypeList<TBases...>*)
{
  static_assert((std::is_base_of_v<TBases, TEvent> && ...),
                "An event's ancestors are its bases");
  (
      [&]
      {
        if constexpr (kKind<TBases> != kKind<TEvent> ||
                      kKind<TEvent> == EventKind::Request ||
                      kKind<TEvent> == EventKind::Response)
          static_cast<void>(sizeof(AncestorOfAnotherKind<TEvent, TBases>));
      }(),
      ...);
  return true;
}

/**
 * Checks that T is an event the engine delivers, at compile time: marked,
 * a request's responses all responses, its ancestors of its own kind. Every
 * call that sends or receives a type instantiates it.
 */
template <typename T>
consteval bool checkEvent()
{
  if constexpr (ResponseQueueType<T>)
    return checkEvent<typename T::Request>();
  else if constexpr (kKind<T> == EventKind::None)
  {
    static_cast<void>(sizeof(NotAnEvent<T>));
    return true;
  }
  else
  {
    if constexpr (kRequest<T>)
      checkResponses<T>(
          static_cast<typename EventTraits<T>::Responses*>(nullptr));
    return checkAncestors<T>(
        static_cast<typename EventTraits<T>::Ancestors*>(nullptr));
  }
}

/** Fails to compile when T, a type to send, is a response. */
template <typename T>
consteval bool checkNotResponse()
{
  if constexpr (kResponse<T>)
    static_cast<void>(sizeof(IsAResponse<T>));
  return true;
}

} // namespace sfs::detail
