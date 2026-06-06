#pragma once

#include "SDL_pixels.h"
#include "config.h"
#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/tags/solidObject.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/assetStore/sprite.h"
#include "engine/runtime/rendering/flatDecal.h"
#include "engine/runtime/systems/flatRenderSystem.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/trigonometric.hpp"

#include <cstddef>
#include <random>
#include <vector>

namespace platformer
{

/**
 * Physics-driven blood. Droplets sprayed at a hit/kill fly with real gravity
 * and stick to the first platform surface they cross, stamping a decal at the
 * impact: a splat sized and oriented by the impact velocity, plus -- where
 * blood hits hard or spills over an edge -- a streak that animates running down
 * the platform face. The accumulated stains are organic because they trace the
 * actual droplet trajectories, the way the isometric game stains terrain from
 * particle-surface collisions, rather than being drawn as a fixed shape.
 */
class BloodSystem : public sfs::System
{
public:
  // FlatRenderSystem the blood decals are stamped into (and which animates the
  // running drips via the decal grow each frame).
  void setRenderer(sfs::FlatRenderSystem* renderer) { m_renderer = renderer; }

  // Soft round sprite (white_dot) for the area-filling splatter blobs.
  void setSprite(sfs::SpriteId sprite) { m_sprite = sprite; }

  // Hard 1x1 sprite (white_pixel) for the thin crisp sub-streak lines.
  void setSolidSprite(sfs::SpriteId sprite) { m_solidSprite = sprite; }

  // Spray `count` sticking droplets from `origin`, fanned around `dir` (pass a
  // zero `dir` for a full radial burst). `up` biases them upward (-Y) so a kill
  // erupts before gravity pulls the blood back down onto the platforms.
  void spray(const glm::vec2& origin,
             const glm::vec2& dir,
             int count,
             float speedMin,
             float speedMax,
             float up,
             const SDL_Color& color,
             float dropSize)
  {
    const bool radial = glm::length(dir) <= 0.01f;
    const float base = radial ? 0.0f : glm::atan(dir.y, dir.x);
    std::uniform_real_distribution<float> fan(-0.9f, 0.9f);
    std::uniform_real_distribution<float> full(0.0f, 6.2831f);
    std::uniform_real_distribution<float> spd(speedMin, speedMax);
    std::uniform_real_distribution<float> sz(0.7f, 1.4f);
    for (int i = 0; i < count; ++i)
    {
      const float a = radial ? full(m_rng) : base + fan(m_rng);
      const float s = spd(m_rng);
      m_drops.push_back(Drop{origin,
                             {glm::cos(a) * s, glm::sin(a) * s - up},
                             dropSize * sz(m_rng),
                             vary(color),
                             2.5f});
    }
  }

protected:
  void update(double deltaTime) override
  {
    if (!registry || !m_renderer || m_sprite == 0)
      return;

    const float dt = static_cast<float>(deltaTime);
    if (dt <= 0.0f)
      return;

    for (std::size_t i = 0; i < m_drops.size();)
    {
      Drop& d = m_drops[i];
      d.vel.y += GRAVITY * dt;
      const glm::vec2 prev = d.pos;
      d.pos += d.vel * dt;
      d.life -= dt;

      if (stickToSurface(d, prev))
      {
        m_drops[i] = m_drops.back();
        m_drops.pop_back();
        continue;
      }
      if (d.life <= 0.0f || d.pos.y > GROUND_Y + 500.0f)
      {
        m_drops[i] = m_drops.back();
        m_drops.pop_back();
        continue;
      }
      ++i;
    }
  }

private:
  struct Drop
  {
    glm::vec2 pos;
    glm::vec2 vel;
    float size;
    SDL_Color color;
    float life;
  };

  // Jitter a blood colour per droplet -- brightness and a little hue wander --
  // so the splatter ranges from dark maroon to brighter red, not one flat tone.
  SDL_Color vary(const SDL_Color& base)
  {
    std::uniform_real_distribution<float> mul(0.7f, 1.15f); // brightness
    std::uniform_int_distribution<int> rj(-25, 18);         // red wander
    std::uniform_int_distribution<int> gbj(-3, 12);         // slight warmth
    const float m = mul(m_rng);
    const auto cl = [](float v)
    { return static_cast<Uint8>(glm::clamp(v, 0.0f, 255.0f)); };
    return SDL_Color{cl((base.r + rj(m_rng)) * m),
                     cl((base.g + gbj(m_rng)) * m),
                     cl((base.b + gbj(m_rng)) * m),
                     base.a};
  }

  // Land the droplet on the first platform surface its path crosses this step.
  // A swept segment-vs-AABB test, so it can't tunnel through thin platforms and
  // it works for ANY face the droplet enters through -- top, sides, or the
  // underside (blood thrown up sticks under a platform). Returns true if it
  // stuck (and stamped).
  bool stickToSurface(const Drop& d, const glm::vec2& prev)
  {
    const glm::vec2 seg = d.pos - prev;
    float bestT = 2.0f;
    glm::vec2 bestEntry{0.0f, 0.0f};
    glm::vec2 bestCenter{0.0f, 0.0f};
    glm::vec2 bestHalf{0.0f, 0.0f};
    bool hit = false;

    for (const auto& solid : registry->view<sfs::SolidObject,
                                            sfs::TransformComponent,
                                            sfs::BoxCollider2D>())
    {
      const glm::vec2 c =
          solid.getComponent<sfs::TransformComponent>().position;
      const glm::vec2 h = solid.getComponent<sfs::BoxCollider2D>().half;
      float tEnter = 0.0f;
      float tExit = 0.0f;
      if (!segmentBox(prev, seg, c - h, c + h, tEnter, tExit))
        continue;
      if (tEnter < 0.0f || tEnter > 1.0f)
        continue; // didn't cross INTO the box this step (or started inside)
      if (tEnter < bestT)
      {
        bestT = tEnter;
        bestEntry = prev + seg * tEnter;
        bestCenter = c;
        bestHalf = h;
        hit = true;
      }
    }

    if (hit)
      stickOnSurface(bestEntry, d, bestCenter, bestHalf);
    return hit;
  }

  // Segment p0 + t*seg vs AABB [lo,hi] (slab method). Sets the entry/exit t
  // along the segment line; returns false if the line misses the box.
  static bool segmentBox(const glm::vec2& p0,
                         const glm::vec2& seg,
                         const glm::vec2& lo,
                         const glm::vec2& hi,
                         float& tEnter,
                         float& tExit)
  {
    float tmin = -1e30f;
    float tmax = 1e30f;
    for (int axis = 0; axis < 2; ++axis)
    {
      const float p = axis == 0 ? p0.x : p0.y;
      const float dr = axis == 0 ? seg.x : seg.y;
      const float l = axis == 0 ? lo.x : lo.y;
      const float hgh = axis == 0 ? hi.x : hi.y;
      if (glm::abs(dr) < 1e-6f)
      {
        if (p < l || p > hgh)
          return false; // parallel to this slab and outside it
        continue;
      }
      float t1 = (l - p) / dr;
      float t2 = (hgh - p) / dr;
      if (t1 > t2)
      {
        const float tmp = t1;
        t1 = t2;
        t2 = tmp;
      }
      tmin = glm::max(tmin, t1);
      tmax = glm::min(tmax, t2);
      if (tmin > tmax)
        return false;
    }
    tEnter = tmin;
    tExit = tmax;
    return true;
  }

  // A soft round blood blob (white_dot) -- the area-filling splatter. Clipped
  // to the platform rectangle [clipMin,clipMax] so it's sliced at the edge and
  // never spills past the surface.
  void blob(float x,
            float y,
            float w,
            float h,
            const SDL_Color& tint,
            const glm::vec2& clipMin,
            const glm::vec2& clipMax)
  {
    sfs::FlatDecal b;
    b.sprite = m_sprite;
    b.worldPos = {x, y};
    b.size = {w, h};
    b.anchor = {0.5f, 0.5f};
    b.tint = tint;
    b.layer = 1;
    b.clip = true;
    b.clipMin = clipMin;
    b.clipMax = clipMax;
    m_renderer->stampDecal(b);
  }

  // A thin crisp blood line (hard white_pixel, ~1px wide) running from `at`
  // along unit `dir` for `len` -- a sub-streak. No bead; just a line.
  void line(const glm::vec2& at,
            const glm::vec2& dir,
            float width,
            float len,
            const SDL_Color& tint)
  {
    const glm::vec2 mid = at + dir * (len * 0.5f);
    const float rot = glm::atan(-dir.x, dir.y); // local +Y -> dir
    sfs::FlatDecal s;
    s.sprite = m_solidSprite;
    s.worldPos = mid;
    s.size = {width, len};
    s.anchor = {0.5f, 0.5f};
    s.rotation = rot;
    s.tint = tint;
    s.layer = 1;
    m_renderer->stampDecal(s);
  }

  // Distance from `at` (on the box surface) along unit `dir` until it exits the
  // box -- so a sub-streak can be clamped to the platform it struck.
  static float boxReach(const glm::vec2& at,
                        const glm::vec2& dir,
                        const glm::vec2& center,
                        const glm::vec2& half)
  {
    const glm::vec2 lo = center - half;
    const glm::vec2 hi = center + half;
    float t = 1e30f;
    if (glm::abs(dir.x) > 1e-6f)
      t = glm::min(t, ((dir.x > 0.0f ? hi.x : lo.x) - at.x) / dir.x);
    if (glm::abs(dir.y) > 1e-6f)
      t = glm::min(t, ((dir.y > 0.0f ? hi.y : lo.y) - at.y) / dir.y);
    return glm::max(t, 0.0f);
  }

  // Stick blood at a surface impact, Noita-style: a dynamic, irregular SPLATTER
  // (a cluster of soft blobs spread along the surface, never poking back out
  // into open air) plus several thin ~1px SUB-STREAKS fanned around the
  // droplet's raw impact direction (any direction -- down a face, sideways, or
  // up under a platform), each clamped to the platform. The splats fill area so
  // enough spray reddens the whole edge; the streaks give it direction.
  void stickOnSurface(const glm::vec2& at,
                      const Drop& d,
                      const glm::vec2& center,
                      const glm::vec2& half)
  {
    const float speed = glm::length(d.vel);
    const glm::vec2 dir = speed > 1.0f ? d.vel / speed : glm::vec2{0.0f, 1.0f};
    const glm::vec2 perp{-dir.y, dir.x};
    const float base = glm::atan(d.vel.y, d.vel.x);
    // Moderate splatter -- big enough that spray accumulates and saturates, not
    // a giant blob. The blobs blend (depth-write off) so they layer cleanly.
    const float splat = d.size * (1.25f + speed * 0.0013f);
    const glm::vec2 clipMin = center - half; // crop to the platform rectangle
    const glm::vec2 clipMax = center + half; // (slices blood at the edges)

    // Irregular splatter: a main blob + satellites spread sideways along the
    // surface. The platform-rect clip keeps it from spilling past any edge, so
    // placement just needs to be roughly on the impact face.
    std::uniform_real_distribution<float> lat(-1.1f, 1.1f);
    std::uniform_real_distribution<float> along(-0.2f, 0.7f);
    std::uniform_real_distribution<float> sz(0.45f, 1.0f);
    std::uniform_int_distribution<int> lobes(4, 7);
    blob(at.x + dir.x * splat * 0.45f,
         at.y + dir.y * splat * 0.45f,
         splat,
         splat,
         d.color,
         clipMin,
         clipMax);
    const int nl = lobes(m_rng);
    for (int i = 0; i < nl; ++i)
    {
      const float r = splat * sz(m_rng);
      const glm::vec2 p =
          at + perp * (lat(m_rng) * splat) + dir * (along(m_rng) * splat);
      blob(p.x, p.y, r, r, d.color, clipMin, clipMax);
    }

    // Sub-streaks: thin lines fanned around the impact direction. More and
    // longer the faster the hit; not every droplet need streak far.
    std::uniform_int_distribution<int> count(2, 5);
    std::uniform_real_distribution<float> fan(-0.45f, 0.45f);
    std::uniform_real_distribution<float> lenf(0.35f, 1.0f);
    std::uniform_real_distribution<float> wid(1.0f, 2.2f);
    const int n = count(m_rng);
    for (int i = 0; i < n; ++i)
    {
      const float a = base + fan(m_rng);
      const glm::vec2 sd{glm::cos(a), glm::sin(a)};
      const float reach = boxReach(at, sd, center, half);
      const float len = glm::min((8.0f + speed * 0.085f) * lenf(m_rng), reach);
      if (len < 4.0f)
        continue;
      line(at, sd, wid(m_rng), len, d.color);
    }
  }

  sfs::FlatRenderSystem* m_renderer = nullptr;
  sfs::SpriteId m_sprite = 0;
  sfs::SpriteId m_solidSprite = 0;
  std::vector<Drop> m_drops;
  std::mt19937 m_rng{4242};
};

} // namespace platformer
