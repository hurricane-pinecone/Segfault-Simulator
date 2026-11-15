#pragma once

#include "engine/core/rendering/debugLines.h"
#include "engine/runtime/rendering/space2D.h"

#include <cstddef>
#include <optional>

namespace sfs
{

class Entity;
class Registry;

/**
 * Colliders as debug lines (see DebugLines): what the physics collides with,
 * where it has it. A 3D collider (BoxCollider3DComponent,
 * SphereCollider3DComponent, CapsuleCollider3DComponent) is placed by its
 * entity's world transform, its size and offset scaled by the transform's
 * scale as the physics bakes them (a sphere stays round); a 2D game's
 * BoxCollider2D (with a Space2D) on its plane, unturned and unscaled. Solid
 * colliders are green, triggers orange.
 *
 * What RenderSystem draws while it draws colliders
 * (RenderSystem::effectiveDrawColliders()), and the editor for the selection.
 */
namespace colliderLines
{

inline constexpr glm::vec4 kSolid{0.35f, 0.9f, 0.47f, 1.0f};
inline constexpr glm::vec4 kTrigger{1.0f, 0.59f, 0.24f, 1.0f};

/**
 * One entity's colliders (none without a TransformComponent). `space`: a
 * 2D game's, for its BoxCollider2D (none in a 3D game: skipped).
 */
void append(const Entity& entity,
            const std::optional<Space2D>& space,
            DebugLines& out);

/**
 * Every entity's colliders.
 * @return how many colliders were drawn
 */
std::size_t appendAll(Registry& registry,
                      const std::optional<Space2D>& space,
                      DebugLines& out);

} // namespace colliderLines

} // namespace sfs
