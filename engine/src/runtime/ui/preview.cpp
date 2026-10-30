#include "engine/runtime/ui/preview.h"

#include <algorithm>

namespace sfs::ui
{

namespace
{
// Previews register as they're made (a game module's, when it loads) and
// leave as they're destroyed (when it unloads).
std::vector<const Preview*>& registry()
{
  static std::vector<const Preview*> all;
  return all;
}

std::vector<const PreviewStyle*>& styleRegistry()
{
  static std::vector<const PreviewStyle*> all;
  return all;
}
} // namespace

Preview::Preview(std::string name,
                 std::function<View()> make,
                 std::source_location where)
    : m_name(std::move(name)), m_make(std::move(make)),
      m_file(where.file_name())
{
  registry().push_back(this);
}

Preview::Preview(std::string name, std::function<View()> make, std::string file)
    : m_name(std::move(name)), m_make(std::move(make)), m_file(std::move(file))
{
  registry().push_back(this);
}

Preview::~Preview() { std::erase(registry(), this); }

const std::vector<const Preview*>& previews() { return registry(); }

PreviewStyle::PreviewStyle(std::function<View(View)> apply,
                           std::source_location where)
    : m_apply(std::move(apply)), m_file(where.file_name())
{
  styleRegistry().push_back(this);
}

PreviewStyle::~PreviewStyle() { std::erase(styleRegistry(), this); }

const std::vector<const PreviewStyle*>& previewStyles()
{
  return styleRegistry();
}

} // namespace sfs::ui
