#pragma once

namespace sfs
{

/**
 * Makes the entity a camera the scene can be seen through, in any render
 * style (add a CameraFollowComponent to have it follow something). The entity's
 * TransformComponent places it, looking along its +X axis with +Z up (the
 * way setYaw faces things). Move the entity to move the view.
 *
 * The scene renders through the first active camera entity (RenderSystem in
 * place of its own camera(), unless told not to: the editor looks around
 * freely while editing; a flat or isometric render system centres on it).
 * Roll is ignored: the view stays level.
 *
 * @param bool active - whether this camera can be the scene's view; turn one
 *                      off and another on to switch cameras
 * @param bool orthographic - parallel projection instead of perspective
 * @param float fieldOfView - vertical, in degrees (perspective)
 * @param float orthoHeight - visible world height (orthographic)
 * @param float zoom - magnification on top of the projection (2 = everything
 *                     twice as big), e.g. for a game's zoom in/out
 * @param float nearPlane - nearest visible distance
 * @param float farPlane - farthest visible distance
 * @param float focusDistance - how far ahead the camera's attention is: the
 *                              sun's shadows are sharpest there, and the point
 *                              lights nearest it are kept when there are too
 *                              many
 * @param bool bloom - light brighter than white glows past its edges, as
 *                     through a real lens (see BloomSettings); how much an
 *                     object blooms is its emission's brightness, x its
 *                     material's Bloom
 * @param float bloomIntensity - how strong the glow is
 * @param float bloomThreshold - how bright light must be to bloom (1: white)
 * @param float bloomSoftness - how gently it eases in above the threshold
 * @param float bloomRadius - how far the glow spreads (1 = normal)
 * @param bool depthOfField - what's nearer or farther than focusDistance
 *                            blurs, as through a real lens (see
 *                            DepthOfFieldSettings)
 * @param float focusRange - how deep the sharp part around the focus is
 * @param float focusBlur - how blurred the farthest things get (1 = normal)
 * @param float focusHyperfocal - the lens's hyperfocal distance: focused this
 *                                far or farther, everything beyond the focus
 *                                is sharp; the sharp part deepens toward it
 *                                as the focus moves off (0: only focusRange)
 */
struct CameraComponent
{
  bool active = true;
  bool orthographic = false;
  [[sfs::range(10, 120)]] [[sfs::showIf(!orthographic)]] float fieldOfView =
      46.0f;
  // No range: a 2D game's world is in pixels (or tiles), a 3D one's in metres.
  [[sfs::showIf(orthographic)]] float orthoHeight = 20.0f;
  [[sfs::range(0.1, 10)]] float zoom = 1.0f;
  [[sfs::range(0.01, 10)]] float nearPlane = 0.1f;
  float farPlane = 500.0f;
  [[sfs::range(1, 200)]] float focusDistance = 20.0f;
  bool bloom = true;
  [[sfs::range(0, 3)]] [[sfs::showIf(bloom)]] float bloomIntensity = 0.6f;
  [[sfs::range(0, 4)]] [[sfs::showIf(bloom)]] float bloomThreshold = 1.0f;
  [[sfs::range(0, 2)]] [[sfs::showIf(bloom)]] float bloomSoftness = 0.5f;
  [[sfs::range(0.25, 3)]] [[sfs::showIf(bloom)]] float bloomRadius = 1.0f;
  bool depthOfField = false;
  [[sfs::range(0, 200)]] [[sfs::showIf(depthOfField)]] float focusRange = 10.0f;
  [[sfs::range(0, 3)]] [[sfs::showIf(depthOfField)]] float focusBlur = 1.0f;
  [[sfs::range(0, 500)]] [[sfs::showIf(depthOfField)]] float focusHyperfocal =
      80.0f;
};

} // namespace sfs
