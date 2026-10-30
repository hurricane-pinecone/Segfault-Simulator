#pragma once

#include "nlohmann/json.hpp"

#include <string>

namespace sfs
{

/**
 * Grass growing on the entity's mesh (its MeshRendererComponent's): real
 * blades, each a curved, tapering strip, on every part of the mesh that faces
 * up (no steeper than 60 degrees). The GPU grows them around the camera each
 * frame, densest near it. They sway in the scene's wind
 * (RenderSystem3D::windSettings()), and flatten where things move through
 * them (rigid bodies that bend grass, and GrassBenderComponent entities),
 * springing back afterwards.
 *
 * How it grows is painted (the editor's grass brush) into maps over the mesh
 * (see GrassMaps): where it grows, how long it is, and data for its material.
 * `paint` holds them as the scene starts; with none it grows everywhere, full
 * length. A game changes them as it plays through GrassModule.
 *
 * @param std::string material - what the blades look like ("asset:<id>" of a
 *        material asset; a node graph sees the blade's UV, across it and up
 *        it, v 1 at the root); empty: a plain green
 * @param float density - blades per square world unit
 * @param float height - a blade's height, in world units
 * @param float heightVariation - how much heights vary (0: all the same)
 * @param float width - a blade's width at its root
 * @param float stiffness - how little it sways in the wind (1: hardly)
 * @param float recovery - seconds flattened grass takes to stand back up
 * @param float drawDistance - how far from the camera grass is drawn; it
 *        thins out towards it
 * @param nlohmann::json paint - the painted maps (GrassMaps::toJson());
 *        edited by the brush, not shown
 */
struct GrassComponent
{
  [[sfs::material]] std::string material;
  [[sfs::range(0, 2000)]] float density = 250.0f;
  [[sfs::range(0.02, 3)]] float height = 0.35f;
  [[sfs::range(0, 1)]] float heightVariation = 0.4f;
  [[sfs::range(0.002, 0.2)]] float width = 0.03f;
  [[sfs::range(0, 1)]] float stiffness = 0.4f;
  [[sfs::range(0.1, 20)]] float recovery = 3.0f;
  [[sfs::range(5, 200)]] float drawDistance = 40.0f;
  nlohmann::json paint = nlohmann::json::object();
};

} // namespace sfs
