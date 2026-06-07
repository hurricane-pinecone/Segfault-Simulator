#pragma once

#include "engine/core/particles/decal.h"
#include "engine/runtime/rendering/flatDecal.h"
#include "engine/runtime/systems/flatRenderSystem.h"
#include "glm/glm/common.hpp"

#include <SDL_pixels.h>

namespace sfs
{

// Adapts the engine's particle decal stream onto the flat 2D decal renderer:
// implement IDecalSink so any ParticleEngine (with a flat collision source) can
// stick particles to flat surfaces and leave marks. Maps the terrain-aware
// DecalSpawn onto a FlatDecal, ignoring the iso-only fields (elevation,
// wallSide, wallBottom) and translating dripSpeed -> a downward grow and
// fadeRate -> lifetime.
//
// Two sprites: a soft one for round marks and a hard (crisp) one for elongated
// streaks, chosen by footprint aspect -- so directional streaks read as sharp
// lines while drops stay soft. Pass the same sprite twice for a single look.
class FlatDecalSink : public IDecalSink
{
public:
  FlatDecalSink(FlatRenderSystem& renderer,
                SpriteId softSprite,
                SpriteId streakSprite,
                int layer = 1)
      : m_renderer(&renderer), m_soft(softSprite), m_streak(streakSprite),
        m_layer(layer)
  {
  }

  void addDecal(const DecalSpawn& spawn) override
  {
    if (!m_renderer)
      return;

    const auto u8 = [](float v)
    { return static_cast<Uint8>(glm::clamp(v, 0.0f, 1.0f) * 255.0f); };

    FlatDecal decal;
    decal.worldPos = spawn.worldPos;
    decal.size = spawn.size;
    decal.rotation = spawn.rotation;
    decal.tint = SDL_Color{u8(spawn.color.r),
                           u8(spawn.color.g),
                           u8(spawn.color.b),
                           u8(spawn.color.a)};
    // Crisp streaks get the hard sprite; round drops the soft one.
    decal.sprite = spawn.crisp ? m_streak : m_soft;
    decal.layer = m_layer;
    decal.clipping = spawn.clipping;
    decal.clipMin = spawn.clipMin;
    decal.clipMax = spawn.clipMax;
    // fadeRate (alpha/sec) -> lifetime (sec); 0 = permanent.
    decal.lifetime = spawn.fadeRate > 0.0f ? 1.0f / spawn.fadeRate : -1.0f;

    // A drip runs down the face: anchor at the top and grow the height over
    // time.
    if (spawn.dripSpeed > 0.0f)
    {
      decal.anchor = {0.5f, 0.0f};
      decal.growTo = {spawn.size.x, spawn.size.y * 5.0f};
      decal.growRate = spawn.dripSpeed;
    }

    m_renderer->stampDecal(decal);
  }

private:
  FlatRenderSystem* m_renderer = nullptr;
  SpriteId m_soft = 0;
  SpriteId m_streak = 0;
  int m_layer = 1;
};

} // namespace sfs
