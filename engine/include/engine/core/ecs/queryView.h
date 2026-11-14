#pragma once

// Query lists read in place, as rows that unpack their components:
//
//   for (auto [entity, position, velocity] :
//        registry->query<Position, Velocity>())
//
// A row converts to its entity, so `for (const sfs::Entity& e : ...)` still
// goes through the entities alone. What a row binds, after the entity:
//
//   a component T            T&, read straight from its slot (the list's
//                            members all have it); const T& for `const T`
//   Any<A, B>                A*, B*, null when the entity has none
//   Excluding<...>           nothing
//   Query<...>               its terms, in order
//
// A loop that doesn't need the entity goes through the components alone. A
// row that binds exactly one component, a required one, converts to it:
//
//   for (Position& position : registry->query<Position, Excluding<Fixed>>())
//
// and `.components()` gives the same rows without their entity:
//
//   for (auto [position, velocity] :
//        registry->query<Position, Velocity>().components())
//
// An Any member binds a pointer that may be null, so a row with one never
// converts: a lone Any<A> lists what A does, and is better written A.
//
// Nothing is fetched for a component the loop doesn't bind, and a binding the
// body never reads costs nothing once optimised. Nothing allocates. Included
// by registry.h; System::getEntities() is defined here too.

#include "engine/core/ecs/component.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/query.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"

#include <cstddef>
#include <iterator>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace sfs
{

namespace detail
{

template <typename... Ts>
struct TypeList
{
};

/** A required component of a row: bound by reference, read from its slot. */
template <typename T>
struct Required
{
  /** The component as the term writes it (const T binds const T&). */
  using Component = T;
};

/** An Any member of a row: bound by pointer, null when absent. */
template <typename T>
struct Optional
{
  /** The component as the term writes it (const T binds const T*). */
  using Component = T;
};

template <typename... TLists>
struct Concat;
template <>
struct Concat<>
{
  using type = TypeList<>;
};
template <typename... Ts>
struct Concat<TypeList<Ts...>>
{
  using type = TypeList<Ts...>;
};
template <typename... Ts, typename... Us, typename... TRest>
struct Concat<TypeList<Ts...>, TypeList<Us...>, TRest...>
{
  using type = typename Concat<TypeList<Ts..., Us...>, TRest...>::type;
};

/**
 * What one query term binds in a row, as a TypeList of Required/Optional. A
 * const group or list binds nothing, so only QueryTerm's error about it shows.
 */
template <typename TTerm>
struct BoundOf
{
  using type = std::conditional_t<isQueryFilter<std::remove_cv_t<TTerm>>,
                                  TypeList<>,
                                  TypeList<Required<TTerm>>>;
};
template <typename... Ts>
struct BoundOf<Any<Ts...>>
{
  using type = TypeList<Optional<Ts>...>;
};
template <typename... Ts>
struct BoundOf<Excluding<Ts...>>
{
  using type = TypeList<>;
};
template <typename... Ts>
struct BoundOf<Query<Ts...>>
{
  using type = typename Concat<typename BoundOf<Ts>::type...>::type;
};

/** What a row of these terms binds after its entity, flattened, in order. */
template <typename... TTerms>
using Bound = typename Concat<typename BoundOf<TTerms>::type...>::type;

/** How a row reads one binding through its Finder. */
template <typename TBinding>
struct Fetch;
template <typename T>
struct Fetch<Required<T>>
{
  using Result = T&;
  static T& get(const Registry::Finder<T>& finder, Entity::EntityId id)
  {
    return finder.held(id);
  }
};
template <typename T>
struct Fetch<Optional<T>>
{
  using Result = T*;
  static T* get(const Registry::Finder<T>& finder, Entity::EntityId id)
  {
    return finder(id);
  }
};

template <typename TBound>
struct FetchersOf;

/**
 * One Finder for each component a row binds, made once where a loop begins
 * (one pool lookup each).
 */
template <typename... TBindings>
struct FetchersOf<TypeList<TBindings...>>
{
  /** How many components a row binds. */
  static constexpr std::size_t count = sizeof...(TBindings);
  /** The bindings, in order. */
  using Bindings = std::tuple<TBindings...>;

  FetchersOf() = default;
  explicit FetchersOf(Registry& registry)
      : finders(registry.template finder<typename TBindings::Component>()...)
  {
  }

  /**
   * The binding I of the living entity `id`.
   * @return T& for a required component, T* for an Any member
   */
  template <std::size_t I>
  decltype(auto) fetch(Entity::EntityId id) const
  {
    using Binding = std::tuple_element_t<I, Bindings>;
    return Fetch<Binding>::get(std::get<I>(finders), id);
  }

  /** The finders, in binding order. */
  std::tuple<Registry::Finder<typename TBindings::Component>...> finders;
};

/** The finders a row of these terms reads its components through. */
template <typename... TTerms>
using Fetchers = FetchersOf<Bound<TTerms...>>;

/** What a row that doesn't bind exactly one component "converts" to. */
struct NoSoleComponent
{
};

/**
 * The component a row converts to: its one binding when that is a required
 * component, else NoSoleComponent.
 */
template <typename TBound>
struct SoleOf
{
  using type = NoSoleComponent;
};
template <typename T>
struct SoleOf<TypeList<Required<T>>>
{
  using type = T;
};

/**
 * What QueryRow and ComponentRow share: the row's entity, the iterator's
 * finders, and the conversion to the row's one component.
 */
template <typename... TTerms>
class RowBase
{
public:
  using Fetch = Fetchers<TTerms...>;
  /** The component the row converts to (NoSoleComponent: none). */
  using Sole = typename SoleOf<Bound<TTerms...>>::type;

  /**
   * A row of `entity` (in the list, or in a walk's iterator), read through
   * `fetch` (the iterator's finders).
   */
  RowBase(const Entity* entity, const Fetch* fetch)
      : m_entity(entity), m_fetch(fetch)
  {
  }

  /**
   * The row's one component, for a row that binds exactly one, a required
   * one: `for (Position& position : registry->query<Position>())`.
   * @return T& for a term T, const T& for `const T`
   */
  operator Sole&() const
    requires(!std::is_same_v<Sole, NoSoleComponent>)
  {
    return m_fetch->template fetch<0>(m_entity->getId());
  }

protected:
  const Entity* m_entity;
  const Fetch* m_fetch;
};

/** The list a QueryView over no list (an empty one) reads. */
inline const std::vector<Entity> kNoEntities;

/**
 * The first of TTerms that a system registered as `registered` can't vouch
 * for (its entities don't all match it), for a log.
 * @return that term's type key, or the whole list's if none fails alone
 */
template <typename... TTerms>
std::string_view unvouchedTerm(const QuerySignature& registered)
{
  std::string_view found;
  const auto test = [&](bool implied, std::string_view term)
  {
    if (found.empty() && !implied)
      found = term;
  };
  (test(registered.implies(makeQuerySignature<TTerms>()),
        componentTypeKey<TTerms>()),
   ...);
  return found.empty() ? componentTypeKey<Query<TTerms...>>() : found;
}

} // namespace detail

/**
 * One entity of a query list or a typed hierarchy walk: converts to the
 * entity, and unpacks as [entity, bound components...] (see the top of this
 * file). A row that binds exactly one component, a required one, converts to
 * that component too. It reads the components only when they're unpacked.
 * It's valid for the loop step that made it.
 */
template <typename... TTerms>
class QueryRow : public detail::RowBase<TTerms...>
{
public:
  using detail::RowBase<TTerms...>::RowBase;

  /** The row's entity, for loops that go through the entities alone. */
  operator const Entity&() const { return *this->m_entity; }
  /** The row's entity. */
  const Entity& entity() const { return *this->m_entity; }

  /**
   * Element I of the row's unpacking.
   * @return 0: the entity; then each bound component in term order, T& for a
   *         required one and T* for an Any member
   */
  template <std::size_t I>
  decltype(auto) get() const
  {
    if constexpr (I == 0)
      return static_cast<const Entity&>(*this->m_entity);
    else
      return this->m_fetch->template fetch<I - 1>(this->m_entity->getId());
  }
};

/**
 * A QueryRow without its entity, as `.components()` gives it: it unpacks as
 * [bound components...], and converts to its one component as a QueryRow
 * does. It's valid for the loop step that made it.
 */
template <typename... TTerms>
class ComponentRow : public detail::RowBase<TTerms...>
{
public:
  using detail::RowBase<TTerms...>::RowBase;

  /** The components of `row`, read through the same finders. */
  explicit ComponentRow(const QueryRow<TTerms...>& row)
      : detail::RowBase<TTerms...>(row)
  {
  }

  /**
   * Element I of the row's unpacking.
   * @return each bound component in term order, T& for a required one and T*
   *         for an Any member
   */
  template <std::size_t I>
  decltype(auto) get() const
  {
    return this->m_fetch->template fetch<I>(this->m_entity->getId());
  }
};

namespace detail
{

template <typename TRow>
struct ComponentRowOf;
template <typename... TTerms>
struct ComponentRowOf<QueryRow<TTerms...>>
{
  using type = ComponentRow<TTerms...>;
};

} // namespace detail

/** The end of a query list's loop. */
struct QuerySentinel
{
  /** One past the list's last entity when the loop began. */
  const Entity* end = nullptr;
};

/**
 * A query list's iterator: its rows are QueryRows. In builds without NDEBUG
 * it checks on each step that the list hasn't changed since the loop began
 * (adding or removing a component the list names, on any entity); if it has,
 * it logs an error naming the query and the systems following its list, and
 * ends the loop rather than read a moved list. Release builds don't check, so
 * such a loop behaves differently there: never change a list's own components
 * while going through it.
 */
template <typename... TTerms>
class QueryIterator
{
public:
  using iterator_concept = std::forward_iterator_tag;
  using iterator_category = std::input_iterator_tag;
  using value_type = QueryRow<TTerms...>;
  using difference_type = std::ptrdiff_t;
  using reference = QueryRow<TTerms...>;

  QueryIterator() = default;
  QueryIterator(Registry& registry, const detail::QueryList& list)
      : m_at(list.entities.data()), m_fetch(registry)
#ifndef NDEBUG
        ,
        m_registry(&registry), m_list(&list), m_version(list.version),
        m_end(list.entities.data() + list.entities.size())
#endif
  {
  }

  /** The row here. */
  QueryRow<TTerms...> operator*() const { return {m_at, &m_fetch}; }
  QueryIterator& operator++()
  {
    ++m_at;
#ifndef NDEBUG
    if (m_list && m_list->version != m_version)
    {
      m_registry->listChangedInLoop(
          *m_list, detail::componentTypeKey<Query<TTerms...>>());
      m_at = m_end;
      m_list = nullptr;
    }
#endif
    return *this;
  }
  QueryIterator operator++(int)
  {
    QueryIterator before = *this;
    ++*this;
    return before;
  }
  bool operator==(const QueryIterator& other) const
  {
    return m_at == other.m_at;
  }
  bool operator==(const QuerySentinel& sentinel) const
  {
    return m_at == sentinel.end;
  }

private:
  const Entity* m_at = nullptr;
  detail::Fetchers<TTerms...> m_fetch;
#ifndef NDEBUG
  const Registry* m_registry = nullptr;
  const detail::QueryList* m_list = nullptr;
  uint32_t m_version = 0;
  const Entity* m_end = nullptr;
#endif
};

/**
 * The rows of a query list or a typed hierarchy walk without their entity,
 * as ComponentRows (QueryView::components(), ChildRows::components(),
 * DescendantRows::components()). It holds the range itself, a 16-byte
 * view, and its iterator wraps the range's: the same finders, made once
 * where the loop begins, and in builds without NDEBUG the same check that a
 * query's list hasn't changed since. Nothing is copied or allocated.
 */
template <typename TRange>
class ComponentView
{
  using RowIterator = decltype(std::declval<const TRange&>().begin());
  using Row =
      typename detail::ComponentRowOf<typename RowIterator::value_type>::type;

public:
  class iterator
  {
  public:
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type = Row;
    using difference_type = std::ptrdiff_t;
    using reference = Row;

    iterator() = default;
    /** At the row the range's iterator `at` is on. */
    explicit iterator(RowIterator at) : m_at(std::move(at)) {}

    /** The row here. */
    Row operator*() const { return Row(*m_at); }
    iterator& operator++()
    {
      ++m_at;
      return *this;
    }
    iterator operator++(int)
    {
      iterator before = *this;
      ++*this;
      return before;
    }
    bool operator==(const iterator& other) const { return m_at == other.m_at; }
    bool operator==(const QuerySentinel& sentinel) const
    {
      return m_at == sentinel;
    }

  private:
    RowIterator m_at;
  };

  /** The rows of `range` (a query's view or a typed walk), held by value. */
  explicit ComponentView(const TRange& range) : m_range(range) {}

  /** The first row. */
  iterator begin() const { return iterator(m_range.begin()); }
  /** Where the rows end: the range's own end. */
  auto end() const
  {
    if constexpr (std::is_same_v<decltype(m_range.end()), RowIterator>)
      return iterator(m_range.end());
    else
      return m_range.end();
  }

private:
  TRange m_range;
};

/**
 * A query's shared list, read in place: two pointers, never a copy
 * (Registry::query(), Scene::query(), System::getEntities()). Going through
 * it gives rows that unpack the terms' components (components(): without the
 * entity); size(), empty(), indexing, front() and back() give entities. It
 * converts to the list itself, so code that stores or passes
 * `const std::vector<Entity>&` takes it as it is. Changing the list while
 * going through it isn't safe: see docs/core/ecs/index.md (go through a
 * copy, `std::vector<Entity>(registry->query<...>())`).
 */
template <typename... TTerms>
class QueryView
{
public:
  using iterator = QueryIterator<TTerms...>;
  using sentinel = QuerySentinel;

  /** An empty view (a system not yet in a registry). */
  QueryView() = default;
  QueryView(Registry* registry, const detail::QueryList* list)
      : m_registry(registry), m_list(list)
  {
  }

  /** The first row; its finders are made here, one pool lookup each. */
  iterator begin() const
  {
    return m_list ? iterator(*m_registry, *m_list) : iterator();
  }
  /** Where the rows end. */
  sentinel end() const
  {
    return {m_list ? m_list->entities.data() + m_list->entities.size()
                   : nullptr};
  }
  /**
   * The same rows without their entity: `for (auto [position, velocity] :
   * registry->query<Position, Velocity>().components())`.
   */
  ComponentView<QueryView> components() const
  {
    return ComponentView<QueryView>(*this);
  }
  /** How many entities the list has. */
  std::size_t size() const { return entities().size(); }
  /** Whether the list has none. */
  bool empty() const { return entities().empty(); }
  /** The list's entity `i` (in id order). */
  const Entity& operator[](std::size_t i) const { return entities()[i]; }
  /** The list's first entity; it must have one. */
  const Entity& front() const { return entities().front(); }
  /** The list's last entity; it must have one. */
  const Entity& back() const { return entities().back(); }

  /**
   * The registry's list itself, for code that stores or passes it (empty for
   * an empty view). It stays valid as long as the registry.
   */
  const std::vector<Entity>& entities() const
  {
    return m_list ? m_list->entities : detail::kNoEntities;
  }
  operator const std::vector<Entity>&() const { return entities(); }

private:
  Registry* m_registry = nullptr;
  const detail::QueryList* m_list = nullptr;
};

template <typename... TTerms>
QueryView<TTerms...> System::getEntities() const
{
  if (m_lists.empty())
    return {};
  if constexpr (sizeof...(TTerms) > 0)
  {
    const QuerySignature& wanted = detail::querySignature<TTerms...>();
    if (!m_signature.implies(wanted))
    {
      static bool logged = false;
      if (!logged)
      {
        logged = true;
        warnNotImplied(detail::unvouchedTerm<TTerms...>(m_signature));
      }
      return {};
    }
  }
  return QueryView<TTerms...>(registry, m_lists.front());
}

} // namespace sfs

template <typename... TTerms>
struct std::tuple_size<sfs::QueryRow<TTerms...>>
    : std::integral_constant<std::size_t,
                             1 + sfs::detail::Fetchers<TTerms...>::count>
{
};

template <typename... TTerms>
struct std::tuple_element<0, sfs::QueryRow<TTerms...>>
{
  using type = const sfs::Entity&;
};

template <std::size_t I, typename... TTerms>
struct std::tuple_element<I, sfs::QueryRow<TTerms...>>
{
  using type = typename sfs::detail::Fetch<std::tuple_element_t<
      I - 1,
      typename sfs::detail::Fetchers<TTerms...>::Bindings>>::Result;
};

template <typename... TTerms>
struct std::tuple_size<sfs::ComponentRow<TTerms...>>
    : std::integral_constant<std::size_t,
                             sfs::detail::Fetchers<TTerms...>::count>
{
};

template <std::size_t I, typename... TTerms>
struct std::tuple_element<I, sfs::ComponentRow<TTerms...>>
{
  using type = typename sfs::detail::Fetch<std::tuple_element_t<
      I,
      typename sfs::detail::Fetchers<TTerms...>::Bindings>>::Result;
};
