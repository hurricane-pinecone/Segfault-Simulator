#pragma once

#include "engine/runtime/input/input.h"
#include "engine/runtime/sceneManager/scene.h"
#include "engine/runtime/ui/layer.h"
#include "engine/runtime/ui/ui.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "voxel/blockRegistry.h"
#include "voxel/voxelGenerator.h"

class Player;

// The HUD: frame rate and time of day, top left.
class GameHud : public sfs::ui::Layer
{
public:
  sfs::ui::View body() override;
};

// A tile under the mouse: which, the height of its ground, and the point on
// it.
struct TilePick
{
  glm::ivec2 tile{0, 0};
  int level = 0;
  glm::vec2 point{0.0f, 0.0f};
};

class GameScene : public sfs::Scene
{
public:
  GameScene(sfs::SceneId id,
            const sfs::SceneServices& services,
            const std::string& name)
      : sfs::Scene(id, services, name)
  {
  }

  double fps() const { return m_fps; }

protected:
  void onInit() override;
  void onProcessInput(const sfs::Input& input) override;
  void onUpdate(double deltaTime) override;
  void onDebugUI() override;

private:
  // The tile under a pixel of the view; false if there's no ground there.
  bool pickTile(glm::vec2 pixel, TilePick& pick);
  void createEntities();

private:
  double m_fps = 0.0;
  // The highest ground there can be: the voxel world's top, in levels.
  static constexpr int kHighestLevel = 16 * sfs::kLevelsPerBlock;
  TilePick m_hovered;
  bool m_hasHoveredTile = false;
  Player* m_player = nullptr;

  GameBlockRegistry m_blockRegistry;
  GameVoxelGenerator m_voxelGenerator;
};
