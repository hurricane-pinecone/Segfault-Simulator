#pragma once

#include "engine/core/components/transformComponent.h"

namespace sfs
{

/** Deprecated: the engine has one transform for every style. */
using Transform3DComponent [[deprecated("use TransformComponent")]] =
    TransformComponent;

} // namespace sfs
