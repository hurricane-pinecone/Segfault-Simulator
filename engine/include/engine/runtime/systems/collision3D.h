#pragma once

// Contact3D lives with ContactReportComponent, which hands contacts to games.
#include "engine/core/components/contactReportComponent.h"
#include "engine/core/ecs/entity.h"

namespace sfs
{

/** What a game object is told when its entity touches another (see GameObject).
 */
struct Collision3D
{
  Entity other; // the entity it touched
};

} // namespace sfs
