#pragma once

#include "engine/core/ecs/component.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/util/hash.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <vector>

namespace sfs
{

/**
 * A query term (Registry::query(), System::registerComponent(),
 * System::observe()): the entity has at least one of TComponents. A query
 * takes at most one Any group; an entity with none of them isn't listed.
 */
template <typename... TComponents>
struct Any;

/**
 * A query term (Registry::query(), System::registerComponent(),
 * System::observe()): the entity has none of TComponents. It wins over the
 * other terms: query<A, Excluding<A>>() lists nothing.
 */
template <typename... TComponents>
struct Excluding;

/**
 * A list of query terms that stands for those terms written out, so a long
 * list can be named once, e.g. in a system:
 * `using Served = sfs::Query<Hitch, WorkOrder, sfs::Excluding<Hidden>>;` and
 * then `registry->query<Served>()`. It takes components, one Any<...> group
 * at most and Excluding<...> groups, and may hold or sit beside other terms,
 * Query lists included: query<Served, Motion>() is query<Hitch, WorkOrder,
 * Excluding<Hidden>, Motion>(), the same signature and the same list. It is
 * flattened at compile time and costs nothing at run time.
 */
template <typename... TTerms>
struct Query;

/**
 * What a query asks of an entity's components: the registry keeps one list
 * per distinct QuerySignature. Built from a query's terms by
 * detail::makeQuerySignature(), in canonical form (canonical()), so queries
 * that list the same entities share one list.
 */
struct QuerySignature
{
  /** The components the entity must all have. */
  Signature all;
  /** The components it must have at least one of; empty: no such term. */
  Signature any;
  /** The components it must have none of. */
  Signature excluding;

  /**
   * Whether an entity with these components belongs in the query's list.
   * @param components the entity's component signature
   * @return true if it has all of `all`, one of `any` (if any) and none of
   *         `excluding`
   */
  bool matches(const Signature& components) const
  {
    return (components & all) == all &&
           (any.none() || (components & any).any()) &&
           (components & excluding).none();
  }

  /**
   * The same query in its one canonical form: an Any group of one component
   * is that component in `all`, and an Any group sharing a component with
   * `all` is always met, so it goes. `excluding` is left as it is: taking it
   * out of `any` would turn an unmeetable Any into none.
   * @return the canonical signature
   */
  QuerySignature canonical() const
  {
    QuerySignature result = *this;
    if (result.any.count() == 1)
    {
      result.all |= result.any;
      result.any.reset();
    }
    else if ((result.any & result.all).any())
      result.any.reset();
    return result;
  }

  /**
   * Whether every entity this signature matches also matches `other`, both
   * canonical: other's required components are all required here, its
   * exclusions all excluded here, and its Any group (if any) is met by a
   * component required here or holds this one's whole Any group.
   * @param other the signature to test against
   * @return true if this one's entities are all `other`'s
   */
  bool implies(const QuerySignature& other) const
  {
    return (other.all & ~all).none() && (other.excluding & ~excluding).none() &&
           (other.any.none() || (other.any & all).any() ||
            (any.any() && (any & ~other.any).none()));
  }

  bool operator==(const QuerySignature&) const = default;
};

/**
 * A query's shared list, read in place (Registry::query(),
 * System::getEntities()): it goes through the entities as rows that unpack
 * their components. See queryView.h.
 */
template <typename... TTerms>
class QueryView;

namespace detail
{

/**
 * Whether T is a filter group (Any or Excluding) or a term list (Query)
 * rather than a component.
 */
template <typename T>
inline constexpr bool isQueryFilter = false;
template <typename... Ts>
inline constexpr bool isQueryFilter<Any<Ts...>> = true;
template <typename... Ts>
inline constexpr bool isQueryFilter<Excluding<Ts...>> = true;
template <typename... Ts>
inline constexpr bool isQueryFilter<Query<Ts...>> = true;

/** Whether T is a term list (Query<...>). */
template <typename T>
inline constexpr bool isQueryList = false;
template <typename... Ts>
inline constexpr bool isQueryList<Query<Ts...>> = true;

/**
 * One query's list, kept by the registry for as long as it lives, in id
 * order. Code reads it through a QueryView.
 */
struct QueryList
{
  /** The matching entities, in id order. */
  std::vector<Entity> entities;
  /**
   * Goes up at every insert and erase, so a loop going through the list can
   * tell it changed under it (checked in debug builds).
   */
  uint32_t version = 0;
};

/**
 * How one query term adds itself to a QuerySignature. This one is a
 * component, which the entity must have; the specialisations below, for Any
 * and Excluding groups and Query lists, have the same two members. A
 * component may be written const (`const Hitch`): it is the same component,
 * and a query's rows bind it as const.
 */
template <typename TComponent>
struct QueryTerm
{
  static_assert(!isQueryFilter<std::remove_cv_t<TComponent>>,
                "const goes on the components inside an sfs::Any, "
                "sfs::Excluding or sfs::Query (sfs::Query<const A, B>), not "
                "on the group or list");
  /** How many Any<...> groups the term holds (a query takes one at most). */
  static constexpr int anyGroups = 0;
  /**
   * Adds the term's components to the signature's all, any or excluding set.
   * @param signature the signature being built
   */
  static void addTo(QuerySignature& signature)
  {
    signature.all.set(Component<TComponent>::getId());
  }
};

template <typename... TComponents>
struct QueryTerm<Any<TComponents...>>
{
  static_assert(sizeof...(TComponents) > 0,
                "sfs::Any<> needs at least one component");
  static_assert(!(isQueryFilter<std::remove_cv_t<TComponents>> || ...),
                "sfs::Any takes components, not filter groups or "
                "sfs::Query lists");
  static constexpr int anyGroups = 1;
  static void addTo(QuerySignature& signature)
  {
    (signature.any.set(Component<TComponents>::getId()), ...);
  }
};

template <typename... TComponents>
struct QueryTerm<Excluding<TComponents...>>
{
  static_assert(sizeof...(TComponents) > 0,
                "sfs::Excluding<> needs at least one component");
  static_assert(!(isQueryFilter<std::remove_cv_t<TComponents>> || ...),
                "sfs::Excluding takes components, not filter groups or "
                "sfs::Query lists");
  static constexpr int anyGroups = 0;
  static void addTo(QuerySignature& signature)
  {
    (signature.excluding.set(Component<TComponents>::getId()), ...);
  }
};

// A Query list adds each of its terms, as if they were written out.
template <typename... TTerms>
struct QueryTerm<Query<TTerms...>>
{
  static constexpr int anyGroups = (0 + ... + QueryTerm<TTerms>::anyGroups);
  static_assert(anyGroups <= 1,
                "an sfs::Query list takes at most one sfs::Any<...> group");
  static void addTo(QuerySignature& signature)
  {
    (QueryTerm<TTerms>::addTo(signature), ...);
  }
};

/**
 * The signature for a query's terms: components (all of them), at most one
 * Any<...> group and any number of Excluding<...> groups, in any order, any
 * of them inside Query<...> lists.
 * @return the signature, in canonical form
 */
template <typename... TTerms>
QuerySignature makeQuerySignature()
{
  static_assert((0 + ... + QueryTerm<TTerms>::anyGroups) <= 1,
                "a query takes at most one sfs::Any<...> group");
  QuerySignature signature;
  (QueryTerm<TTerms>::addTo(signature), ...);
  return signature.canonical();
}

/**
 * makeQuerySignature() for these terms, built once on first use and kept:
 * what the registry's queries, the systems' typed lists, the typed hierarchy
 * walks and Entity::matches() test with. Its thread-safe static is shared by
 * every caller of the same terms in a module.
 * @return the terms' canonical signature, valid for the program's lifetime
 */
template <typename... TTerms>
const QuerySignature& querySignature()
{
  static const QuerySignature signature = makeQuerySignature<TTerms...>();
  return signature;
}

} // namespace detail

} // namespace sfs

/**
 * Hashes a QuerySignature for the registry's map of query lists. The filter
 * sets are mixed in only when they are used, so a plain all-of query costs one
 * bitset hash.
 */
template <>
struct std::hash<sfs::QuerySignature>
{
  std::size_t operator()(const sfs::QuerySignature& signature) const noexcept
  {
    const std::hash<sfs::Signature> hashOf;
    uint64_t hash = hashOf(signature.all);
    // Salted, so the same set as `any` or as `excluding` hashes apart.
    if (signature.any.any())
      hash = sfs::hashCombine(hash, sfs::mix64(hashOf(signature.any) ^ 1u));
    if (signature.excluding.any())
      hash =
          sfs::hashCombine(hash, sfs::mix64(hashOf(signature.excluding) ^ 2u));
    return static_cast<std::size_t>(hash);
  }
};
