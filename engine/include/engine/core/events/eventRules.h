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

/** How many types a TypeList has. */
template <typename TList>
inline constexpr std::size_t kListSize = 0;
template <typename... TTypes>
inline constexpr std::size_t kListSize<TypeList<TTypes...>> = sizeof...(TTypes);

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

/** Fails to compile, naming a request's role type that isn't a response. */
template <typename TRequest, typename TResponse>
struct NotAResponse
{
  static_assert(sizeof(TResponse) == 0,
                "A request's roles name only response types "
                "(NotAResponse<Request, Type>): mark it "
                "[[sfs::event_response]]");
};

/** A response's role in its request. */
enum class ResponseRole : uint8_t
{
  /** The success response: final. */
  Success,
  /** A progress response: partial, any number of them. */
  Progress,
  /** The failure response: final. */
  Failure,
};

// A request's response type in a role; void for a type that isn't a
// request, whose traits have no roles, so the check naming that comes first.
template <typename TRequest, ResponseRole kRole, bool = kRequest<TRequest>>
struct RoleTypeOf
{
  using Type = void;
};
template <typename TRequest, ResponseRole kRole>
struct RoleTypeOf<TRequest, kRole, true>
{
  using Type = std::conditional_t<
      kRole == ResponseRole::Success,
      typename EventTraits<TRequest>::Success,
      std::conditional_t<kRole == ResponseRole::Progress,
                         typename EventTraits<TRequest>::Progress,
                         typename EventTraits<TRequest>::Failure>>;
};

/**
 * A request's response type in a role (void when the role isn't declared,
 * or the type isn't a request).
 */
template <typename TRequest, ResponseRole kRole>
using RoleType = typename RoleTypeOf<TRequest, kRole>::Type;

/** Fails to compile, naming a request and a type that isn't its success. */
template <typename TRequest, typename TType>
struct NotTheSuccessResponse
{
  static_assert(sizeof(TType) == 0,
                "respond() sends the request's success response (the first "
                "type of [[sfs::event_request(Success, ...)]]); this type "
                "(NotTheSuccessResponse<Request, Type>) isn't it");
};

/** Fails to compile, naming a request with no progress role. */
template <typename TRequest>
struct RequestHasNoProgress
{
  static_assert(sizeof(TRequest) == 0,
                "progress() needs the request to declare a progress "
                "response ([[sfs::event_request(Success, progress = P)]]); "
                "this one (RequestHasNoProgress<Request>) doesn't");
};

/** Fails to compile, naming a request and a type that isn't its progress. */
template <typename TRequest, typename TType>
struct NotTheProgressResponse
{
  static_assert(sizeof(TType) == 0,
                "progress() sends the request's progress response; this type "
                "(NotTheProgressResponse<Request, Type>) isn't it");
};

/** Fails to compile, naming a request with no failure role. */
template <typename TRequest>
struct RequestHasNoFailure
{
  static_assert(sizeof(TRequest) == 0,
                "fail() needs the request to declare a failure response "
                "([[sfs::event_request(Success, failure = F)]]); this one "
                "(RequestHasNoFailure<Request>) doesn't");
};

/** Fails to compile, naming a request and a type that isn't its failure. */
template <typename TRequest, typename TType>
struct NotTheFailureResponse
{
  static_assert(sizeof(TType) == 0,
                "fail() sends the request's failure response; this type "
                "(NotTheFailureResponse<Request, Type>) isn't it");
};

/**
 * Checks, at compile time, that a server sends `TType` in a role of
 * `TRequest` it declares: respond() its success, progress() its progress,
 * fail() its failure. A broken rule fails to compile, naming the types.
 * @return whether it may (always true once it compiles)
 */
template <typename TRequest, ResponseRole kRole, typename TType>
consteval bool checkRoleSent()
{
  using Declared = RoleType<TRequest, kRole>;
  if constexpr (kRole == ResponseRole::Progress && std::is_void_v<Declared>)
    static_cast<void>(sizeof(RequestHasNoProgress<TRequest>));
  else if constexpr (kRole == ResponseRole::Failure && std::is_void_v<Declared>)
    static_cast<void>(sizeof(RequestHasNoFailure<TRequest>));
  else if constexpr (!std::is_same_v<Declared, TType>)
  {
    if constexpr (kRole == ResponseRole::Success)
      static_cast<void>(sizeof(NotTheSuccessResponse<TRequest, TType>));
    else if constexpr (kRole == ResponseRole::Progress)
      static_cast<void>(sizeof(NotTheProgressResponse<TRequest, TType>));
    else
      static_cast<void>(sizeof(NotTheFailureResponse<TRequest, TType>));
  }
  return true;
}

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

// A role's type, when declared, is a response.
template <typename TRequest, typename TType>
consteval bool checkRoleType()
{
  if constexpr (std::is_void_v<TType>)
    return true;
  else
    return checkResponseType<TRequest, TType>();
}

// A request's roles: a success, optional progress and failure, each a
// response, no type in two roles.
template <typename TRequest>
consteval bool checkRoles()
{
  using Traits = EventTraits<TRequest>;
  using Success = typename Traits::Success;
  using Progress = typename Traits::Progress;
  using Failure = typename Traits::Failure;
  static_assert(!std::is_void_v<Success>,
                "A request declares its success response: "
                "[[sfs::event_request(Success, progress = P, failure = F)]]");
  static_assert(
      !std::is_same_v<Success, Progress> && !std::is_same_v<Success, Failure> &&
          (std::is_void_v<Progress> || !std::is_same_v<Progress, Failure>),
      "A request gives each response type one role");
  return checkRoleType<TRequest, Success>() &&
         checkRoleType<TRequest, Progress>() &&
         checkRoleType<TRequest, Failure>();
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
      checkRoles<T>();
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
