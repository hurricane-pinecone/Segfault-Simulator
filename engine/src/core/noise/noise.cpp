#include "engine/core/noise/noise.h"

#include <FastNoiseLite/FastNoiseLite.h>

#include <memory>

namespace sfs
{

class Noise::Impl
{
public:
  FastNoiseLite noise;
};

Noise& Noise::operator=(Noise&&) noexcept = default;

Noise::Noise() : m_impl(std::make_unique<Impl>())
{
  setSeed(1337);
  setType(Type::OpenSimplex);
  setFrequency(0.03f);
}

Noise::Noise(Noise&&) noexcept = default;

Noise::~Noise() = default;

float Noise::get(float x, float y) const
{
  return m_impl->noise.GetNoise(x, y);
}

void Noise::setFrequency(float frequency)
{
  m_impl->noise.SetFrequency(frequency);
}

void Noise::setSeed(int seed) { m_impl->noise.SetSeed(seed); }

void Noise::setType(Type type)
{
  switch (type)
  {
  case Type::OpenSimplex:
    m_impl->noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    break;

  case Type::Perlin:
    m_impl->noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    break;
  }
}

} // namespace sfs
