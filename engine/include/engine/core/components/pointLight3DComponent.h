#pragma once

#include "engine/core/components/pointLightComponent.h"

namespace sfs
{

/** Deprecated: the engine has one point light for every style. */
using PointLight3DComponent [[deprecated("use PointLightComponent")]] =
    PointLightComponent;

} // namespace sfs
