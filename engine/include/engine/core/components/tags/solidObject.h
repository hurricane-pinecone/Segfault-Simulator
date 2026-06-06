#pragma once

namespace sfs
{

// Tags an entity as a static, immovable solid that other bodies collide with
// (paired with a collider -- WorldCollider for solid-vs-solid on the iso ground
// plane, BoxCollider2D for the flat path's platforms).
struct SolidObject
{
};

} // namespace sfs
