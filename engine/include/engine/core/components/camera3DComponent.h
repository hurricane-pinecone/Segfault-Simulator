#pragma once

#include "engine/core/components/cameraComponent.h"

namespace sfs
{

/** Deprecated: the engine has one camera for every style. */
using Camera3DComponent [[deprecated("use CameraComponent")]] = CameraComponent;

} // namespace sfs
