#include "engine/runtime/rendering/visualStyles.h"

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering/renderSystem.h"

#include <algorithm>

namespace sfs
{

std::vector<std::pair<std::string, std::string>>
visualStyleAssets(const AssetDatabase& assets)
{
  // The records are sorted by path already.
  std::vector<std::pair<std::string, std::string>> out;
  for (const AssetRecord& record : assets.records())
    if (record.kind == AssetKind::VisualStyle && record.id.valid())
      out.emplace_back(
          "asset:" + record.id.toString(), record.path.generic_string());
  return out;
}

std::string
cycleVisualStyle(RenderSystem& render, const AssetDatabase& assets, int step)
{
  const auto styles = visualStyleAssets(assets);
  if (styles.empty())
  {
    LOG_INFO("The project has no visual styles to switch between");
    return {};
  }
  const auto current =
      std::find_if(styles.begin(),
                   styles.end(),
                   [&](const auto& style)
                   { return style.first == render.visualStyleName(); });
  const int count = static_cast<int>(styles.size());
  int next = 0;
  if (current != styles.end())
    next = (static_cast<int>(current - styles.begin()) + step % count + count) %
           count;
  else if (step < 0)
    next = count - 1;
  render.setVisualStyle(styles[next].first);
  LOG_INFO("Visual style: " + styles[next].second);
  return styles[next].second;
}

} // namespace sfs
