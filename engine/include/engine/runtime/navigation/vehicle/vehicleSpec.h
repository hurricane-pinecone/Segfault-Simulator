#pragma once

#include "engine/core/assets/scriptableObject.h"

#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/**
 * A vehicle model's physical properties, shared by every machine of the
 * model (an .asset; a VehicleComponent picks it): its geometry, steering,
 * engine, brakes, mass and gearing. Properties only, never what physics
 * makes of them: how fast it speeds up, brakes and corners is derived from
 * these, the rig's whole mass (what it tows and carries, which a game
 * reports to NavigationSystem) and the grip under it (a game's tyres x
 * surface): NavigationSystem::setRigProvider. A wheelbase of 0 means
 * "measured": the rig's (from the model) is used.
 *
 * A game extends it with its own kinds of machine:
 *
 *   struct TractorSpec : sfs::VehicleSpec
 *   {
 *     float hitchHeight = 0.5f; // ...
 *   };
 *
 * and an AssetRef<sfs::VehicleSpec> takes a TractorSpec asset too.
 *
 * @param float wheelbase - m, fixed (rear) axle to the steered axle
 * @param float maxSteerAngle - degrees the steered wheels turn at most
 * @param float steerRate - degrees a second the steered wheels turn
 * @param float topSpeed - m/s, the most its gearing gives
 * @param float roadSpeed - m/s it travels at between jobs (at most topSpeed)
 * @param float reverseSpeed - m/s, the most backing up
 * @param float power - kW at the wheels
 * @param float mass - kg, the machine alone (empty, nothing hitched)
 * @param float brakeForce - kN its brakes hold back at most
 * @param float shuttleSeconds - driving: s it stands at a change between
 *        forward and reverse (its wheel goes on turning as it moves off)
 * @param glm::vec3 axle - the middle of its fixed axle in the model's frame:
 *        the point that turns on a circle (the model faces its +X)
 * @param bool mayReverse - whether it can back up at all
 */
struct VehicleSpec : ScriptableObject
{
  [[sfs::range(0.3, 10)]] float wheelbase = 2.6f;
  [[sfs::range(5, 60)]] float maxSteerAngle = 27.5f;
  [[sfs::range(5, 360)]] float steerRate = 69.0f;
  [[sfs::range(0.5, 60)]] float topSpeed = 11.0f;
  [[sfs::range(0.5, 60)]] float roadSpeed = 7.0f;
  [[sfs::range(0.1, 10)]] float reverseSpeed = 2.0f;
  [[sfs::range(1, 1000)]] float power = 75.0f;
  [[sfs::range(50, 50000)]] float mass = 5000.0f;
  [[sfs::range(0.5, 500)]] float brakeForce = 25.0f;
  [[sfs::range(0, 5)]] float shuttleSeconds = 0.5f;
  glm::vec3 axle{0.0f};
  bool mayReverse = true;
};

} // namespace sfs
