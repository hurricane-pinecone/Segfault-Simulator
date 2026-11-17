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
  Noise(Noise&&) noexcept;
  Noise(const Noise&) = delete;
  ~Noise();
  Noise& operator=(Noise&&) noexcept;
  Noise& operator=(const Noise&) = delete;
  float get(float x, float y) const;
  void setFrequency(float frequency);
  void setSeed(int seed);
  void setType(Type type);

private:
  class Impl;

  std::unique_ptr<Impl> m_impl;
};

} // namespace sfs
