#pragma once

namespace sfs
{

/**
 * Makes the entity something the cursor can pick (see CursorPickComponent and
 * pickPickable()). A hit on any mesh at or below it counts as it, through the
 * nearest pickable entity at or above the mesh: put it on a machine's root and
 * a click on any of its parts picks the machine. Meshes with no pickable
 * entity above them are skipped and don't block the ray.
 *
 * @param bool enabled - whether it can be picked now (off: its meshes are
 *        skipped, as if it had none)
 */
struct PickableComponent
{
  bool enabled = true;
};

} // namespace sfs
