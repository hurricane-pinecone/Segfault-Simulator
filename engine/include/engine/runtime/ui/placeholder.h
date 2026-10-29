#pragma once

#include "engine/runtime/ui/ui.h"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

// Stand-ins for what a view shows, so the editor's UI Preview can show a view
// struct (ClockPanel, JobBoard) with nothing filled in: its text fields their
// names ("Time"), its pictures soft boxes, its lists two items each, its
// optionals one and its variants the alternative they hold. Numbers,
// flags and actions keep their defaults, as does anything already set. The
// preview code sfs-codegen writes for a project's *.ui.h files fills each
// view struct's fields with these.
namespace sfs::ui
{

/**
 * How a `T` is filled in: left as it is, unless specialised (below for text,
 * views, lists, optionals and variants; by sfs-codegen for each struct in a
 * .ui.h).
 * `name` is the field's ("Time"), `item` its place in a list (1, 2...; 0 when
 * it's not in one).
 */
template <typename T>
struct Placeholder
{
  static void fill(T&, std::string_view /*name*/, int /*item*/) {}
};

/** `field` filled in as a `name`d field (see Placeholder). */
template <typename T>
void placeholder(T& field, std::string_view name, int item)
{
  Placeholder<T>::fill(field, name, item);
}

template <>
struct Placeholder<std::string>
{
  static void fill(std::string& text, std::string_view name, int item)
  {
    if (!text.empty())
      return;
    text = std::string(name);
    if (item > 0)
      text += " " + std::to_string(item);
  }
};

template <>
struct Placeholder<View>
{
  static void fill(View& view, std::string_view, int)
  {
    if (view.node()->kind == Node::Kind::Empty)
      view = Image::placeholder();
  }
};

template <typename T, typename Allocator>
struct Placeholder<std::vector<T, Allocator>>
{
  static void fill(std::vector<T, Allocator>& items, std::string_view name, int)
  {
    if constexpr (std::is_default_constructible_v<T>)
      if (items.empty())
      {
        items.resize(2);
        for (int i = 0; i < 2; ++i)
          placeholder(items[i], name, i + 1);
      }
  }
};

template <typename T>
struct Placeholder<std::optional<T>>
{
  static void fill(std::optional<T>& value, std::string_view name, int item)
  {
    if constexpr (std::is_default_constructible_v<T>)
      if (!value)
        placeholder(value.emplace(), name, item);
  }
};

template <typename... Ts>
struct Placeholder<std::variant<Ts...>>
{
  static void fill(std::variant<Ts...>& value, std::string_view name, int item)
  {
    std::visit([&](auto& held) { placeholder(held, name, item); }, value);
  }
};

/**
 * A `T` (a view struct) made with nothing set, then filled in; a note in its
 * place if it can't be made so.
 */
template <typename T>
View placeholderView()
{
  if constexpr (!ViewStruct<T>)
    return Text("Not a view: its body() needs to be const and return a View");
  else if constexpr (!std::is_default_constructible_v<T>)
    return Text("Can't be made without values: give it a preview "
                "(sfs::ui::Preview)");
  else
  {
    T value{};
    placeholder(value, "", 0);
    return View(value);
  }
}

} // namespace sfs::ui
