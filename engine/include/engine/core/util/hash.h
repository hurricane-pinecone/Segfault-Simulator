#pragma once

#include <cstdint>
#include <string_view>

namespace sfs
{

/** FNV-1a 64's starting state (fnv1a64()). */
inline constexpr uint64_t kFnv1a64Offset = 14695981039346656037ull;

/**
 * FNV-1a 64 over some bytes, continuing from a state, so a key can be hashed
 * in parts (the same as hashing them joined).
 *
 * @param bytes the bytes to add
 * @param state the hash so far (kFnv1a64Offset to start)
 * @return the hash with the bytes added
 */
constexpr uint64_t fnv1a64(std::string_view bytes,
                           uint64_t state = kFnv1a64Offset)
{
  for (const char c : bytes)
    state = (state ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  return state;
}

/**
 * splitmix64's finaliser: spreads every bit of a value over the whole word,
 * e.g. to finish a hash whose low bits are kept.
 *
 * @param value the value to mix
 * @return the mixed value
 */
constexpr uint64_t mix64(uint64_t value)
{
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

/**
 * Combines a value into a hash (boost's hash_combine step, 64-bit), e.g. to
 * hash a key made of several parts. Mix weak values (mix64()) first.
 *
 * @param seed the hash so far
 * @param value the value to add
 * @return the combined hash
 */
constexpr uint64_t hashCombine(uint64_t seed, uint64_t value)
{
  return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
}

} // namespace sfs
