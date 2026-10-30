#pragma once

namespace sfs
{

/**
 * Tag: a static solid (with a BoxCollider2D) that particles bounce off and
 * stain (see ColliderParticleCollision). PhysicsSystem2D stops bodies against
 * any static BoxCollider2D, tagged or not.
 */
struct SolidObject
{
};

} // namespace sfs
