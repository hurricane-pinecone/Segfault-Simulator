#pragma once
#include <sfs/sfs.h>

#include <cmath>
#include <vector>

// A backdrop that scrolls slower than the world, for depth: its entity's
// sprite, repeated side by side across the view, carried along by `follow` of
// the camera's movement (1: fixed on the screen, like the sky; 0: moves with
// the world).
class Parallax : public sfs::GameObject
{
public:
  float follow = 0.8f;   // how much of the camera's movement it shares
  float width = 2048.0f; // one copy's width (world units)
  int copies = 4;        // side by side (enough to cover the view)
  float height = 0.0f;   // how far it rises from the camera's centre

protected:
  void onCreate(sfs::Scene& scene) override
  {
    m_camera = scene.findEntity("Camera");
    // The rest of the row: the same look, side by side.
    for (int i = 1; i < copies; ++i)
    {
      sfs::Entity copy = scene.createEntity();
      copy.addComponent<sfs::TransformComponent>(
          entity().getComponent<sfs::TransformComponent>());
      copy.addComponent<sfs::SpriteComponent>(
          entity().getComponent<sfs::SpriteComponent>());
      if (entity().hasComponent<sfs::RenderLayerComponent>())
        copy.addComponent<sfs::RenderLayerComponent>(
            entity().getComponent<sfs::RenderLayerComponent>());
      m_copies.push_back(copy);
    }
  }

  void onUpdate(double) override
  {
    if (!m_camera.isValid())
      return;
    const glm::vec2 camera =
        m_camera.getComponent<sfs::TransformComponent>().position2D();
    // The row moves `follow` as far as the camera; its copies are laid from
    // the one under the camera, half of them each side.
    const float origin = camera.x * follow;
    const float under = std::floor((camera.x - origin) / width);
    const float first =
        origin + (under - static_cast<float>(copies / 2)) * width;
    const float y = camera.y - height;
    entity().getComponent<sfs::TransformComponent>().setPosition2D({first, y});
    for (std::size_t i = 0; i < m_copies.size(); ++i)
      m_copies[i].getComponent<sfs::TransformComponent>().setPosition2D(
          {first + width * (i + 1), y});
  }

private:
  sfs::Entity m_camera;
  std::vector<sfs::Entity> m_copies;
};
