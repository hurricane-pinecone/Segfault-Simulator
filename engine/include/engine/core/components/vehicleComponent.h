#pragma once

#include "engine/core/assets/scriptableObject.h"

namespace sfs
{

struct VehicleSpec; // engine/runtime/navigation/vehicle/vehicleSpec.h

/**
 * Makes the entity a vehicle of a model: its VehicleSpec (an asset of a
 * type derived from it too, a game's TractorSpec say). NavigationSystem
 * drives an agent of kind "vehicle" (NavAgentComponent) by it.
 *
 * @param sfs::AssetRef<sfs::VehicleSpec> spec - the vehicle's model
 */
struct VehicleComponent
{
  AssetRef<sfs::VehicleSpec> spec;
};

} // namespace sfs
