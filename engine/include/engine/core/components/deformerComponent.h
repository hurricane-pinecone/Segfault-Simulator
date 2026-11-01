#pragma once

#include <string>

namespace sfs
{

/**
 * Presses into deformable ground (a terrain with TerrainMaterial::
 * deformable) wherever its meshes touch it: tyres leave tracks with their
 * lugs in them, an implement's tines cut furrows, a roller flattens. It
 * covers the entity's own mesh (its MeshRendererComponent's) and, with
 * `children`, its children's: put one on a tractor prefab's root and its
 * wheels and implement all deform the ground (its body never reaches it).
 *
 * Each frame it moves, its meshes' undersides (seen from below the ground)
 * press the ground under them down to `sinkDepth` below where they are
 * (DeformationModule): the ground never rises back by itself, so its tracks
 * stay, deeper where it's gone over twice. Soil pressed aside is pushed up
 * in a low ridge along them. Rest a vehicle on the ground's surface as it
 * is (don't lower it by its sink: it would only sink further each pass).
 *
 * @param bool enabled - whether it deforms the ground now (e.g. off while an
 *        implement's raised)
 * @param float sinkDepth - how far it sinks into the softest ground
 *        (metres): about its weight on its footprint
 * @param float softnessResponse - how much the soil's softness (see
 *        DeformationModule::setSoftness(); wet mud soft, dry ground hard)
 *        changes how far it sinks: 0 always sinkDepth, 1 sinkDepth x the
 *        softness
 * @param float ridge - how much of the soil it presses out is pushed up
 *        alongside (0: it's all compacted)
 * @param bool children - whether its children's meshes deform too
 * @param std::string tread - for smooth tyres: an image whose red is a
 *        pattern pressed in under it (1 deepest), laid on the ground along
 *        its +x, `treadSize` across; empty: none (lugs modelled on the tyre
 *        press in by themselves)
 * @param float treadDepth - how deep the tread pattern presses (metres)
 * @param float treadSize - how far the pattern spans before it repeats
 */
struct DeformerComponent
{
  bool enabled = true;
  [[sfs::range(0, 0.4)]] float sinkDepth = 0.05f;
  [[sfs::range(0, 1)]] float softnessResponse = 0.8f;
  [[sfs::range(0, 1)]] float ridge = 0.5f;
  bool children = true;
  [[sfs::texture]] std::string tread;
  [[sfs::range(0, 0.1)]] float treadDepth = 0.02f;
  [[sfs::range(0.05, 4)]] float treadSize = 0.5f;
};

} // namespace sfs
