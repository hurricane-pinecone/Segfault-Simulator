#pragma once

#include <memory>
#include <string>

namespace sfs
{

class RenderTexture;

/**
 * A camera that films the scene into a small picture a UI shows (a tractor
 * terminal's rear camera, a combine's grain tank camera): its `picture` is
 * what it films, and ui::Image(feed.picture) shows it. The
 * entity's TransformComponent places it, looking along its +X axis with +Z
 * up, as a CameraComponent's: make it a child of a vehicle (or of one of its
 * parts) to ride along.
 *
 * It's filmed only while something on screen shows its picture, `rate`
 * times a second (one feed a frame), through a cheaper view than the
 * scene's: the scene's lighting, sun and lamp shadows, terrain, its
 * deformation (ruts, furrows) and grass's ground, without bloom, depth of
 * field, ambient occlusion, anti-aliasing over time or rain's drops.
 * Nothing shows it: it costs nothing.
 *
 * @param bool enabled - whether it films (off: its picture keeps the last)
 * @param std::string label - its name for a UI ("REAR", "TANK")
 * @param float fieldOfView - vertical, in degrees (75: a wide-angle lens,
 *        about 110 degrees across at 16:9)
 * @param int width - its picture's width, in pixels
 * @param int height - its picture's height, in pixels
 * @param float rate - how many pictures a second
 * @param float nearPlane - nearest it sees
 * @param float farPlane - farthest it sees
 * @param bool grassBlades - grass's blades too (grown again for it, which
 *        costs more); off, only the green of the ground under them
 * @param float grassReach - how far from it blades grow (with grassBlades)
 */
struct CameraFeedComponent
{
  bool enabled = true;
  std::string label = "CAM";
  [[sfs::range(10, 150)]] float fieldOfView = 75.0f;
  [[sfs::range(16, 1280)]] int width = 320;
  [[sfs::range(16, 720)]] int height = 180;
  [[sfs::range(1, 60)]] float rate = 15.0f;
  [[sfs::range(0.01, 10)]] float nearPlane = 0.05f;
  float farPlane = 200.0f;
  bool grassBlades = false;
  [[sfs::range(5, 100)]] [[sfs::showIf(grassBlades)]] float grassReach = 30.0f;

  /**
   * The picture it films into, for a UI to show: made by RenderSystem's
   * update for every feed (null before that), and resized with width and
   * height.
   */
  [[sfs::hide]] std::shared_ptr<RenderTexture> picture;
  /** When it was last filmed (seconds, steady clock; RenderSystem's). */
  [[sfs::hide]] double filmed = -1.0e9;
};

} // namespace sfs
