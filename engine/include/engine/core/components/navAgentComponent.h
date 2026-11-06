#pragma once

#include <cstdint>
#include <string>

namespace sfs
{

/**
 * Makes the entity an AI agent that NavigationSystem plans for and moves:
 * send it somewhere with NavigationSystem::requestGoal() and it finds the
 * way round the scene's colliders, drives it and gives way to others. Its
 * boxes are its own BoxCollider3DComponents (on it and the entities below
 * it): one truth, the colliders. Give it a kinematic RigidBody3DComponent
 * so the physics sees it move.
 *
 * Navigation only: how it searches, what it collides with, its right of
 * way and how it prices a route. How it moves comes from elsewhere: a
 * "vehicle" (a car, a tractor, a rig towing a trailer: it turns on a
 * circle, reverses at a cost, follows its planned path exactly) needs a
 * VehicleComponent with its VehicleSpec.
 *
 * @param std::string kind - its agent kind ("vehicle")
 * @param std::string search - the path search, by name ("hybridAStar")
 * @param float reversePenalty - route cost: a second backing up costs this
 *        many
 * @param float effort - driving style: the share of what its engine, brakes
 *        and the grip allow that it uses speeding up, braking and cornering
 *        (1: flat out; small: gentle)
 * @param uint32_t layers - the colliders' layers it collides with (bits)
 * @param int priority - right of way (higher first: a working machine)
 */
struct NavAgentComponent
{
  std::string kind = "vehicle";
  std::string search = "hybridAStar";
  [[sfs::range(1, 5)]] float reversePenalty = 1.5f;
  [[sfs::range(0.05, 1)]] float effort = 0.25f;
  uint32_t layers = 0xFFFFFFFFu;
  int priority = 0;
};

} // namespace sfs
