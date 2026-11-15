#pragma once

#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>
#include <limits>
#include <optional>
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
 * circle, backs up at its reverse speed, follows its planned path exactly,
 * every way priced by the time it takes) needs a
 * VehicleComponent with its VehicleSpec.
 *
 * @param std::string kind - its agent kind ("vehicle")
 * @param std::string search - the path search, by name ("hybridAStar")
 * @param float effort - driving style: the share of what its engine, brakes
 *        and the grip allow that it uses speeding up, braking and cornering
 *        (1: flat out; small: gentle)
 * @param uint32_t layers - the colliders' layers it collides with (bits)
 * @param int priority - rank in traffic (higher reserves first: a working
 * machine)
 */
/**
 * A towed thing on an agent's hitch (a trailer, an implement on its
 * drawbar): the game hitches and unhitches (NavigationSystem::hitch()), the
 * system then plans and drives the rig and writes the towed entity's
 * transform. Angles are degrees its drawbar may swing going forward /
 * backing up.
 */
struct NavHitch
{
  Entity towed;
  float hitchBehind = 1.0f; // m, the hitch behind the agent's axle
  float drawbar = 4.0f;     // m, the towed thing's axle behind the hitch
  // m its axle (where it pivots, its wheels) is ahead of its own origin
  // (negative: behind): its colliders are measured from there, and its
  // transform written at that much behind the axle's place.
  float axle = 0.0f;
  float forwardLimit = 83.0f, reverseLimit = 35.0f;
  // Mounted (on a linkage or front mount): it moves with the agent, its
  // colliders part of the agent's boxes, never swinging (the hitch numbers
  // unused), measured by NavigationSystem::mountedBox().
  bool rigid = false;
  // A mounted part's pose (its root's) in the agent's own frame (the
  // agent entity's origin and axes, level: no body pitch or roll), as its
  // mount holds it in the state it's driven in now (a header raised for
  // travel, lowered working): the game sets it from its authored mount
  // and hitches again when the state changes. None: where the part is held
  // now, relative to the agent (still in the agent's own frame).
  std::optional<glm::mat4> mount;
};

struct NavAgentComponent
{
  std::string kind = "vehicle";
  std::string search = "hybridAStar";
  [[sfs::range(0.05, 1)]] float effort = 0.25f;
  uint32_t layers = 0xFFFFFFFFu;
  int priority = 0;

  // Live state, set by the game through NavigationSystem (hitch(),
  // setSpeedCap(), setActive()): what it tows; the fastest it may drive
  // (m/s; 0 stands it; infinity: no cap); whether it's an agent now (false:
  // carried by something else, switched off: not planned, moved or claimed).
  [[sfs::hide]] std::optional<NavHitch> hitch;
  [[sfs::hide]] float speedCap = std::numeric_limits<float>::infinity();
  [[sfs::hide]] bool active = true;
  // As NavigationSystem last gave it to its fleet: its reference point (its
  // frame's axle), what it towed and the layers it collided with (any
  // changed: its boxes measured again); and why it's left out of the fleet,
  // if it is (logged once).
  [[sfs::hide]] glm::vec3 givenAxle{0.0f};
  [[sfs::hide]] std::optional<NavHitch> givenHitch;
  [[sfs::hide]] uint32_t givenLayers = 0;
  [[sfs::hide]] std::string complaint;
};

} // namespace sfs
