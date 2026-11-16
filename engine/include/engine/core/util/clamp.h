#pragma once

#include <algorithm>

namespace sfs
{

/**
 * A value clamped to 0..1, NaN to 0: for amounts read from data a game or a
 * file sets (how hard it rains, how wet things are).
 *
 * @param value the value
 * @return it in 0..1
 */
inline float unitInterval(float value)
{
  return value > 0.0f ? std::min(value, 1.0f) : 0.0f;
}

} // namespace sfs
