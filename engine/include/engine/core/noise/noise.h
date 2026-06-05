#pragma once

#include <memory>

namespace sfs
{

class Noise
{
public:
  enum class Type
  {
    OpenSimplex,
    Perlin
  };

  Noise();
  ~Noise();

  Noise(Noise&&) noexcept;
  Noise& operator=(Noise&&) noexcept;

  Noise(const Noise&) = delete;
  Noise& operator=(const Noise&) = delete;

  void setSeed(int seed);
  void setFrequency(float frequency);
  void setType(Type type);

  float get(float x, float y) const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace sfs
