#pragma once

#include "engine/core/components/elevationComponent.h"
#include "engine/core/components/worldCollider.h"
#include "engine/core/rendering/iTerrainHeightSource.h"
#include <engine/core/components/rigidBodyComponent.h>
#include <engine/core/components/transformComponent.h>
#include <engine/core/ecs/system.h>
#include <glm/glm/common.hpp>
#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

class MovementSystem : public System
{
public:
  MovementSystem()
  {
    registerComponent<TransformComponent>();
    registerComponent<RigidBodyComponent>();
  }

  ~MovementSystem() = default;

  // Terrain heights used to block movement into cliffs. Without a source,
  // entities move freely with no terrain constraint.
  void setTerrainHeightSource(const ITerrainHeightSource* source)
  {
    m_terrainHeightSource = source;
  }

  void update(double deltaTime) override
  {
    for (const auto& entity : getEntities())
    {
      auto& rb = entity.getComponent<RigidBodyComponent>();
      auto& transform = entity.getComponent<TransformComponent>();

      transform.previousPosition = transform.position;

      const glm::vec2 desired =
          transform.position + rb.velocity * static_cast<float>(deltaTime);

      if (m_terrainHeightSource)
      {
        // Resolve against the world-collider footprint so the entity stops with
        // its leading edge at a cliff, not its centre (else half the body sinks
        // in).
        glm::vec2 offset{0.0f};
        glm::vec2 size{0.0f};
        if (entity.hasComponent<WorldCollider>())
        {
          const auto& collider = entity.getComponent<WorldCollider>();
          offset = collider.worldOffset();
          size = collider.worldSize();
        }

        transform.position =
            resolveTerrainStep(transform.position, desired, offset, size, rb);
      }
      else
      {
        transform.position = desired;
      }

      if (m_terrainHeightSource && entity.hasComponent<ElevationComponent>())
      {
        auto& elevation = entity.getComponent<ElevationComponent>();
        elevation.level = terrainHeightAt(transform.position);
      }

      if (entity.hasComponent<WorldCollider>())
      {
        auto& collider = entity.getComponent<WorldCollider>();
        collider.updateBounds(transform.position);
      }
    }
  }

private:
  // Tallest step (in elevation levels) an entity can climb in one move; a
  // larger rise is a cliff that blocks movement. Stepping down any amount is
  // allowed.
  static constexpr int kMaxClimb = 2;

  int terrainHeightAt(const glm::vec2& position) const
  {
    return m_terrainHeightSource->terrainHeightAt(
        static_cast<int>(glm::floor(position.x)),
        static_cast<int>(glm::floor(position.y)));
  }

  bool isCliff(float worldX, float worldY, int fromLevel) const
  {
    return terrainHeightAt(glm::vec2{worldX, worldY}) - fromLevel > kMaxClimb;
  }

  // Resolve movement against terrain steps per axis, sampling the collider's
  // leading edge (both corners) so a wide body is blocked before it overlaps a
  // cliff. Resolving X then Y lets the entity slide along a face diagonally.
  glm::vec2 resolveTerrainStep(const glm::vec2& current,
                               const glm::vec2& desired,
                               const glm::vec2& colliderOffset,
                               const glm::vec2& colliderSize,
                               RigidBodyComponent& rb) const
  {
    const int currentLevel = terrainHeightAt(current);

    glm::vec2 result = current;

    if (desired.x != current.x)
    {
      const float edgeX = desired.x + colliderOffset.x +
                          (desired.x > current.x ? colliderSize.x : 0.0f);
      const float yLo = current.y + colliderOffset.y;
      const float yHi = yLo + colliderSize.y;

      if (isCliff(edgeX, yLo, currentLevel) ||
          isCliff(edgeX, yHi, currentLevel))
        rb.velocity.x = 0.0f;
      else
        result.x = desired.x;
    }

    if (desired.y != current.y)
    {
      const float edgeY = desired.y + colliderOffset.y +
                          (desired.y > current.y ? colliderSize.y : 0.0f);
      const float xLo = result.x + colliderOffset.x;
      const float xHi = xLo + colliderSize.x;

      if (isCliff(xLo, edgeY, currentLevel) ||
          isCliff(xHi, edgeY, currentLevel))
        rb.velocity.y = 0.0f;
      else
        result.y = desired.y;
    }

    return result;
  }

  const ITerrainHeightSource* m_terrainHeightSource = nullptr;
};

} // namespace sfs
