#pragma once

#include "engine/core/rendering/mesh.h"

namespace sfs
{

/**
 * Levels of detail for the entity's MeshRendererComponent: simpler meshes
 * drawn in its mesh's place farther from the camera, and last a billboard
 * impostor, so a wood can stretch to the horizon as real trees. A change of
 * level cross-fades by dithering for `fadeSeconds` (a masked or foliage
 * material's pixels of one level giving way to the next's; others switch at
 * once), then it's drawn solid: nothing stays dithered at any distance.
 * Distances are from the camera to the entity's origin, x its largest scale
 * axis (a tree scaled up 1.5 keeps its detail 1.5 times as far). A level
 * without a mesh is skipped; with none it draws its own mesh everywhere.
 *
 * The impostor is a quad in the mesh's x = 0 plane facing +x (its pivot on
 * the ground under the middle, like the model's), turned about the entity's
 * up axis to face the camera; its material's textures hold `impostorFrames`
 * views of the model in a row, from all round it (frame k seen from the
 * model's own +x turned 360 k / frames degrees anticlockwise), and it shows the
 * one nearest the view's direction.
 *
 * Shadows are cast from level `shadowLevel` while a coarser level isn't
 * drawn (a cheaper outline in the shadow map; 0: each level's own), without
 * fading; impostors cast none unless `impostorShadows` (then the last mesh
 * level casts in their place).
 *
 * @param MeshHandle lod1 - the next level's mesh (empty: none)
 * @param float lod1Distance - where it takes over (m, x scale)
 * @param MeshHandle lod2 - the level after it
 * @param float lod2Distance - where that takes over
 * @param MeshHandle lod3 - the level after that
 * @param float lod3Distance - where that takes over
 * @param MeshHandle impostor - the billboard drawn past all of them
 * @param float impostorDistance - where it takes over
 * @param int impostorFrames - how many views round the model its texture holds
 * @param bool impostorShadows - whether far ones cast a shadow (as the last
 *                               mesh level)
 * @param float hysteresis - how far past a level's ends (m, x scale) the
 *                           camera goes before it changes, so it doesn't
 *                           flick back and forth on a boundary
 * @param float fadeSeconds - how long a change of level cross-fades over (0:
 *                            at once)
 * @param int shadowLevel - the level (0 the mesh's own, 1 lod1...) that casts
 *                          its shadow while it's drawn finer
 * @param float cullDistance - past this nothing's drawn (m, x scale; 0: never)
 */
struct MeshLodComponent
{
  MeshHandle lod1;
  [[sfs::range(0, 2000)]] float lod1Distance = 40.0f;
  MeshHandle lod2;
  [[sfs::range(0, 2000)]] float lod2Distance = 100.0f;
  MeshHandle lod3;
  [[sfs::range(0, 2000)]] float lod3Distance = 200.0f;
  MeshHandle impostor;
  [[sfs::range(0, 4000)]] float impostorDistance = 250.0f;
  [[sfs::range(1, 32)]] int impostorFrames = 8;
  bool impostorShadows = false;
  [[sfs::range(0, 100)]] float hysteresis = 4.0f;
  [[sfs::range(0, 2)]] float fadeSeconds = 0.3f;
  [[sfs::range(0, 3)]] int shadowLevel = 1;
  [[sfs::range(0, 10000)]] float cullDistance = 0.0f;
};

} // namespace sfs
