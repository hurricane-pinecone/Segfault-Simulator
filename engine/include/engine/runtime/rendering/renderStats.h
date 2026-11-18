#pragma once

#include <cstddef>

namespace sfs
{

/** What the last frame drew, for HUDs and profiling. */
struct RenderStats
{
  std::size_t drawCalls = 0;
  std::size_t instances = 0;
  std::size_t shadowInstances = 0;
  std::size_t pointLights = 0; // all of them, emission's included
  std::size_t emissionLights = 0;
  std::size_t shadowedLights = 0;       // point lights casting shadows
  std::size_t lightShadowInstances = 0; // casters for them
  std::size_t grassFields = 0;
  std::size_t colliders = 0;  // drawn as debug lines (drawColliders)
  std::size_t debugLines = 0; // every debug line, colliders' included
};

} // namespace sfs
