#pragma once

#include <cstdint>

namespace sfs::random
{

/**
 * The next number from the engine's random source: one per process (a
 * splitmix64 sequence), seeded from the system's entropy when first used, safe
 * to call from any thread. For ids that must not repeat across runs or
 * machines (a scene entry's $sceneId), not for gameplay randomness, which
 * keeps its own seeded generators.
 *
 * @return 64 random bits
 */
uint64_t next();

/**
 * Restart the source from `seed`, so the numbers that follow are the same on
 * every run (for tests).
 *
 * @param seed any value
 */
void seed(uint64_t seed);

} // namespace sfs::random
