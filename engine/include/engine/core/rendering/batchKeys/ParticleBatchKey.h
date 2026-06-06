#pragma once

#include "engine/core/types/blendMode.h"

namespace sfs
{

// Buckets particle geometry into one draw call per (texture, blend mode,
// depth-test state) -- the renderer's particle batcher uses it as a std::map key.
struct ParticleBatchKey
{
  unsigned int texture = 0;
  BlendMode blend = BlendMode::Alpha;
  bool depthTested = true;

  bool operator<(const ParticleBatchKey& other) const
  {
    if (texture != other.texture)
      return texture < other.texture;
    if (blend != other.blend)
      return blend < other.blend;
    return depthTested < other.depthTested;
  }
};

} // namespace sfs
