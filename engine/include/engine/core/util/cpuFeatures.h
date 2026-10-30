#pragma once

#include <string>

namespace sfs
{

/**
 * The instruction set extensions this build of the engine needs that the CPU
 * (or the OS, for AVX's registers) lacks, e.g. "AVX2, FMA"; empty when it has
 * them all. On x86-64 desktop builds the 3D physics library (Jolt, from Conan)
 * is compiled for AVX2, so a CPU without it would crash with an illegal
 * instruction as soon as physics ran: a program checks this first, and says
 * so instead (see the player's and the editor's main).
 */
std::string missingCpuFeatures();

} // namespace sfs
