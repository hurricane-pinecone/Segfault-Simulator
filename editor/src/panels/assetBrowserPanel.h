#pragma once

#include "panels/panel.h"
#include "scripting/scriptTemplates.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Browses the project's content: scenes, scripts and assets as a folder tree.
// Double-click a scene to open it, a script or other text file to edit it in
// the Code panel, and anything else to open it in the system's default
// application; right-click for New Script and Show in Folder.
//
// Listings are cached and refreshed every couple of seconds (or on demand),
// since walking folders every frame is slow on network/WSL shares.
class AssetBrowserPanel : public Panel
{
public:
  struct Actions
  {
    std::function<void(const std::filesystem::path&)> openScene;
    std::function<void(const std::filesystem::path&)> openText;
    std::function<void(scriptTemplates::Kind)> newScript;
  };

  explicit AssetBrowserPanel(Actions actions)
      : Panel("Assets"), m_actions(std::move(actions))
  {
  }

protected:
  void drawContents(EditorContext& context) override;

private:
  struct Node
  {
    std::filesystem::path path;
    bool directory = false;
    std::vector<Node> children;
  };

  void refresh(const std::filesystem::path& root);
  void drawNode(const Node& node);
  void drawContextMenu(const std::filesystem::path& folder);
  void activate(const std::filesystem::path& file);

  Actions m_actions;
  std::filesystem::path m_root;
  std::vector<Node> m_folders;
  std::chrono::steady_clock::time_point m_lastRefresh;
};
