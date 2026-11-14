#pragma once

#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/rendering/camera.h"
#include "engine/core/rendering/lighting.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/trigonometric.hpp"

namespace sfs
{

// The scene's camera: its first active one (CameraComponent +
// TransformComponent, in id order), or an invalid entity when there's none.
// RenderSystem draws through it and CameraSystem moves it.
inline Entity findActiveCamera(Registry& registry)
{
  for (auto [entity, camera, transform] :
       registry.query<CameraComponent, TransformComponent>())
    if (camera.active)
      return entity;
  return {};
}

// Point `view` the way `camera`, with world matrix `world`, looks.
inline void
viewThrough(const CameraComponent& camera, const glm::mat4& world, Camera& view)
{
  view.setPose(glm::vec3(world[3]),
               glm::vec3(world[0]),
               camera.focusDistance,
               glm::vec3(world[2]));
  view.projection = camera.orthographic ? Camera::Projection::Orthographic
                                        : Camera::Projection::Perspective;
  const float magnify = glm::max(camera.zoom, 0.01f);
  view.fieldOfView =
      2.0f *
      glm::atan(glm::tan(glm::radians(camera.fieldOfView) * 0.5f) / magnify);
  view.orthoHeight = camera.orthoHeight / magnify;
  view.nearPlane = camera.nearPlane;
  view.farPlane = camera.farPlane;
}

// The bloom and the depth of field seen through `camera`.
inline BloomSettings bloomOf(const CameraComponent& camera)
{
  return {camera.bloom,
          camera.bloomIntensity,
          camera.bloomThreshold,
          camera.bloomSoftness,
          camera.bloomRadius};
}

inline DepthOfFieldSettings depthOfFieldOf(const CameraComponent& camera)
{
  return {camera.depthOfField,
          camera.focusRange,
          camera.focusBlur,
          camera.focusHyperfocal};
}

} // namespace sfs
