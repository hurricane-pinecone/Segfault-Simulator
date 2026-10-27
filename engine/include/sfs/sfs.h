#pragma once

// Everything a game script usually needs, in one include. Scripts written from
// the editor's templates start with `#include <sfs/sfs.h>`.

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/nameComponent.h"
#include "engine/core/components/pointLight3DComponent.h"
#include "engine/core/components/scriptComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: export
#include "engine/core/logger/logger.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/input/input.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/meshSource.h"
#include "engine/runtime/rendering3d/renderSystem3D.h"
#include "engine/runtime/sceneManager/scene.h"

#include "glm/glm/ext/quaternion_float.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include "glm/glm/geometric.hpp"

#include <string>
