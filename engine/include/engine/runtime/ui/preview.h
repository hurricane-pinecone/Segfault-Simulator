#pragma once

#include "engine/runtime/ui/ui.h"

#include <functional>
#include <source_location>
#include <string>
#include <vector>

namespace sfs::ui
{

/**
 * A view with sample data, shown by the editor's UI Preview while you write
 * the file it's in (like SwiftUI's #Preview): no need to Play. Every view
 * struct in a .ui.h shows there anyway, filled with placeholders (see
 * placeholder.h); declare a preview to see one with real-looking values, or
 * in another state. Declare them at namespace scope, beside the views they
 * show:
 *
 *   static const sfs::ui::Preview badgePreview{
 *       "Badge", [] { return Badge{.text = "New"}; }};
 *
 * The preview is as big as the view it makes; a view as wide as it can be
 * (maxWidth(infinity), a ZStack) gets the panel's width. A preview is in the
 * running game too, but costs nothing there: only the editor makes its view.
 */
class Preview
{
public:
  Preview(std::string name,
          std::function<View()> make,
          std::source_location where = std::source_location::current());
  /** One for `file` (sfs-codegen's, for a .ui.h's view structs). */
  Preview(std::string name, std::function<View()> make, std::string file);
  ~Preview();

  Preview(const Preview&) = delete;
  Preview& operator=(const Preview&) = delete;

  const std::string& name() const { return m_name; }
  /** Its view, made now. */
  View make() const { return m_make ? m_make() : View(); }
  /** The source file it's declared in (as it was compiled). */
  const std::string& file() const { return m_file; }

private:
  std::string m_name;
  std::function<View()> m_make;
  std::string m_file;
};

/** Every preview the loaded code declares, in the order they were made. */
const std::vector<const Preview*>& previews();

/**
 * How the previews of the file it's in are shown (its views' placeholders,
 * and its declared previews): in the look the game puts them in, such as its
 * font. Declare one in a .ui.cpp:
 *
 *   static const sfs::ui::PreviewStyle farmLook{
 *       [](sfs::ui::View view) { return look(view); }};
 */
class PreviewStyle
{
public:
  explicit PreviewStyle(
      std::function<View(View)> apply,
      std::source_location where = std::source_location::current());
  ~PreviewStyle();

  PreviewStyle(const PreviewStyle&) = delete;
  PreviewStyle& operator=(const PreviewStyle&) = delete;

  /** `view` in this style. */
  View apply(View view) const
  {
    return m_apply ? m_apply(std::move(view)) : view;
  }
  /** The source file it's declared in (as it was compiled). */
  const std::string& file() const { return m_file; }

private:
  std::function<View(View)> m_apply;
  std::string m_file;
};

/** Every preview style the loaded code declares. */
const std::vector<const PreviewStyle*>& previewStyles();

} // namespace sfs::ui
