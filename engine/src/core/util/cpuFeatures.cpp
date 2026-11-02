#include "engine/core/util/cpuFeatures.h"

// SFS_REQUIRE_AVX2 is defined (engine/CMakeLists.txt) where the engine links a
// Jolt built for AVX2. This file itself is built for the baseline, so it runs
// on any x86-64 CPU.
#if defined(SFS_REQUIRE_AVX2) && (defined(__x86_64__) || defined(_M_X64))
  #if defined(_MSC_VER)
    #include <immintrin.h>
    #include <intrin.h>
  #else
    #include <cpuid.h>
  #endif
  #include <cstdint>
#endif

namespace sfs
{

#if defined(SFS_REQUIRE_AVX2) && (defined(__x86_64__) || defined(_M_X64))

namespace
{
struct Registers
{
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
};

Registers cpuid(uint32_t leaf, uint32_t subleaf = 0)
{
  Registers r;
  #if defined(_MSC_VER)
  int values[4] = {};
  __cpuidex(values, static_cast<int>(leaf), static_cast<int>(subleaf));
  r.eax = static_cast<uint32_t>(values[0]);
  r.ebx = static_cast<uint32_t>(values[1]);
  r.ecx = static_cast<uint32_t>(values[2]);
  r.edx = static_cast<uint32_t>(values[3]);
  #else
  __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
  #endif
  return r;
}

// Which register state the OS saves on a task switch (XCR0).
uint64_t enabledRegisterState()
{
  #if defined(_MSC_VER)
  return _xgetbv(0);
  #else
  uint32_t low = 0, high = 0;
  __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
  return (static_cast<uint64_t>(high) << 32) | low;
  #endif
}

bool bit(uint32_t value, int index) { return (value >> index) & 1u; }
} // namespace

std::string missingCpuFeatures()
{
  std::string missing;
  const auto need = [&](bool present, const char* name)
  {
    if (present)
      return;
    if (!missing.empty())
      missing += ", ";
    missing += name;
  };

  const uint32_t maxLeaf = cpuid(0).eax;
  const uint32_t maxExtended = cpuid(0x80000000u).eax;
  const Registers basic = cpuid(1);
  const Registers extended = maxLeaf >= 7 ? cpuid(7, 0) : Registers{};
  const Registers amd =
      maxExtended >= 0x80000001u ? cpuid(0x80000001u) : Registers{};

  // AVX needs the OS to save the YMM registers (XCR0 bits 1 and 2).
  const bool osSavesYmm =
      bit(basic.ecx, 27) && (enabledRegisterState() & 0x6) == 0x6;

  need(bit(basic.ecx, 19), "SSE4.1");
  need(bit(basic.ecx, 20), "SSE4.2");
  need(bit(basic.ecx, 28) && osSavesYmm, "AVX");
  need(bit(extended.ebx, 5) && osSavesYmm, "AVX2");
  need(bit(basic.ecx, 12), "FMA");
  need(bit(basic.ecx, 29), "F16C");
  need(bit(extended.ebx, 3), "BMI1");
  need(bit(amd.ecx, 5), "LZCNT");
  need(bit(basic.ecx, 23), "POPCNT");
  return missing;
}

#else

std::string missingCpuFeatures() { return {}; }

#endif

} // namespace sfs
