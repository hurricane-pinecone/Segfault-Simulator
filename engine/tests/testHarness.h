#pragma once

// Shared, dependency-free harness for the engine-core test executables. Each
// test is its own program: it CHECKs conditions and returns report()'s exit
// code (non-zero on any failure), which CTest records as pass/fail.

#include <cmath>
#include <cstdio>

namespace testing
{
inline int g_passed = 0;
inline int g_failures = 0;

inline void check(bool cond, const char* expr, const char* file, int line)
{
  if (cond)
  {
    ++g_passed;
  }
  else
  {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
  }
}

inline bool approx(float a, float b, float eps = 1e-4f)
{
  return std::fabs(a - b) <= eps;
}

inline int report(const char* name)
{
  std::fprintf(
      stderr, "%s: %d passed, %d failed\n", name, g_passed, g_failures);
  return g_failures == 0 ? 0 : 1;
}
} // namespace testing

// Variadic so a condition containing commas (e.g. view<A, B>()) isn't split into
// multiple macro arguments by the preprocessor.
#define CHECK(...)                                                             \
  ::testing::check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
