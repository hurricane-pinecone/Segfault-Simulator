#pragma once

#include <cstddef>
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

/**
 * A fast hash of a few 64-bit words, to notice that they changed: two
 * independent multiply-xorshift chains (the even words and the odd ones), so
 * their steps overlap. Each step is a bijection of its chain for a given
 * word, so two inputs that differ in one word never collide; others collide
 * at about 2^-64.
 *
 * @param words the words
 * @param count how many there are
 * @return their hash
 */
constexpr uint64_t hashWords(const uint64_t* words, std::size_t count)
{
  uint64_t even = 0x9E3779B97F4A7C15ull;
  uint64_t odd = 0xD1B54A32D192ED03ull;
  for (std::size_t word = 0; word < count; word += 2)
  {
    even = (even ^ words[word]) * 0xBF58476D1CE4E5B9ull;
    even ^= even >> 31;
    if (word + 1 < count)
    {
      odd = (odd ^ words[word + 1]) * 0x94D049BB133111EBull;
      odd ^= odd >> 29;
    }
  }
  return even ^ odd;
}

} // namespace sfs
