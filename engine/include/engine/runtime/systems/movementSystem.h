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

      // An actor with an ElevationComponent carries a real (cave-aware) height,
      // so its walls/floors are sampled at its current depth -- it can be
      // inside a cave below the surface. `cave` only turns on once the actor is
      // placed, which waits for real terrain (an unstreamed column reads as the
      // void floor and must not bury the actor at spawn).
      bool cave = false;

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

        int fromLevel = terrainHeightAt(transform.position);
        if (entity.hasComponent<ElevationComponent>())
        {
          auto& elevation = entity.getComponent<ElevationComponent>();
          if (elevation.level == EmptyElevation)
          {
            // Place on the surface, but only once the column has streamed in
            // (a missing column reads as the void floor, level 0).
            if (fromLevel > 0)
            {
              elevation.height = static_cast<float>(fromLevel);
              elevation.level = fromLevel;
              cave = true;
            }
          }
          else
          {
            cave = true;
            fromLevel = elevation.level; // snapped, so exact
          }
        }

        transform.position = resolveTerrainStep(
            transform.position, desired, offset, size, rb, fromLevel);
      }
      else
      {
        transform.position = desired;
      }

      if (cave)
      {
        auto& elevation = entity.getComponent<ElevationComponent>();
        updateElevation(elevation, transform.position);
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
  // allowed (the actor then falls to the floor below).
  static constexpr int kMaxClimb = 2;
  // Headroom (levels) an actor needs to stand/move: roughly one block tall.
  static constexpr int kBodyClearance = 2;

  int terrainHeightAt(const glm::vec2& position) const
  {
    return m_terrainHeightSource->terrainHeightAt(
        static_cast<int>(glm::floor(position.x)),
        static_cast<int>(glm::floor(position.y)));
  }

  // The floor + block test an actor at `fromLevel` gets at (x,y): the surface,
  // a climbable step, or a cave floor when below ground -- and whether a wall
  // or low ceiling stops it. This unified query is why surface cliffs still
  // block while cave tunnels stay walkable.
  WalkableFloor floorAt(float worldX, float worldY, int fromLevel) const
  {
    return m_terrainHeightSource->walkableFloor(
        static_cast<int>(glm::floor(worldX)),
        static_cast<int>(glm::floor(worldY)),
        fromLevel,
        kMaxClimb,
        kBodyClearance);
  }

  // Snap the actor to the floor it stands on (its surface, a step it climbed,
  // or a cave floor it dropped onto). Snapping -- not easing -- keeps the
  // gameplay elevation exact, so the cliff checks above sample the actor's true
  // level and can't spuriously read a step as a wall (the render eases the
  // visual itself).
  void updateElevation(ElevationComponent& elevation,
                       const glm::vec2& position) const
  {
    const int floor = floorAt(position.x, position.y, elevation.level).floor;
    elevation.height = static_cast<float>(floor);
    elevation.level = floor;
  }

  // Resolve movement against terrain steps per axis, sampling the collider's
  // leading edge (both corners) so a wide body is blocked before it overlaps a
  // cliff. Resolving X then Y lets the entity slide along a face diagonally.
  glm::vec2 resolveTerrainStep(const glm::vec2& current,
                               const glm::vec2& desired,
                               const glm::vec2& colliderOffset,
                               const glm::vec2& colliderSize,
                               RigidBodyComponent& rb,
                               int fromLevel) const
  {
    glm::vec2 result = current;

    if (desired.x != current.x)
    {
      const float edgeX = desired.x + colliderOffset.x +
                          (desired.x > current.x ? colliderSize.x : 0.0f);
      const float yLo = current.y + colliderOffset.y;
      const float yHi = yLo + colliderSize.y;

      if (floorAt(edgeX, yLo, fromLevel).blocked ||
          floorAt(edgeX, yHi, fromLevel).blocked)
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

      if (floorAt(xLo, edgeY, fromLevel).blocked ||
          floorAt(xHi, edgeY, fromLevel).blocked)
        rb.velocity.y = 0.0f;
      else
        result.y = desired.y;
    }

    return result;
  }

  const ITerrainHeightSource* m_terrainHeightSource = nullptr;
};

} // namespace sfs
