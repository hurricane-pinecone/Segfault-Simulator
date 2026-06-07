#pragma once

// Lightweight test bed for the engine-core test executables (dependency-free,
// no GoogleTest). A program groups its checks into named TESTs and ends with
// report():
//
//   TEST("the thing should do its job")
//   {
//     CHECK(thing() == 42);
//   }
//   return testing::report("myTests");
//
// CHECK decomposes a comparison so a failure prints the operand values. Each
// TEST prints its pass/fail tally, then lists its own failures beneath it;
// report() prints the program total and returns the exit code (non-zero on any
// failure) for CTest. Output is coloured when stderr is a terminal, plain
// otherwise.

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>

// isatty/fileno (terminal detection for coloured output) live in <unistd.h> on
// POSIX and in <io.h> as _isatty/_fileno on MSVC.
#if defined(_WIN32)
  #include <io.h>
#else
  #include <unistd.h>
#endif

namespace testing
{
struct Stats
{
  int passed = 0;
  int failed = 0;
};

namespace detail
{
inline Stats g_total;

// Failures are buffered while inside a TEST and flushed under its header line,
// so they read beneath the test they belong to rather than streaming above it.
inline int g_testDepth = 0;
inline std::string g_buffer;

inline bool colorEnabled()
{
#if defined(_WIN32)
  static const bool on = _isatty(_fileno(stderr)) != 0;
#else
  static const bool on = ::isatty(fileno(stderr)) != 0;
#endif
  return on;
}

inline void emit(const std::string& text)
{
  if (g_testDepth > 0)
    g_buffer += text;
  else
    std::fputs(text.c_str(), stderr);
}

inline void flushBuffer()
{
  if (!g_buffer.empty())
  {
    std::fputs(g_buffer.c_str(), stderr);
    g_buffer.clear();
  }
}
} // namespace detail

inline const char* cPass() { return detail::colorEnabled() ? "\033[32m" : ""; }
inline const char* cFail() { return detail::colorEnabled() ? "\033[31m" : ""; }
inline const char* cDim() { return detail::colorEnabled() ? "\033[2m" : ""; }
inline const char* cReset() { return detail::colorEnabled() ? "\033[0m" : ""; }

// --- value stringification (for the failure detail) ------------------------
inline std::string stringify(bool v) { return v ? "true" : "false"; }
inline std::string stringify(const std::string& v) { return "\"" + v + "\""; }
inline std::string stringify(const char* v)
{
  return std::string("\"") + (v ? v : "") + "\"";
}
template <std::size_t N>
std::string stringify(const char (&v)[N])
{
  return std::string("\"") + v + "\"";
}
inline std::string stringify(const glm::vec2& v)
{
  return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ")";
}
inline std::string stringify(const glm::ivec2& v)
{
  return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ")";
}
template <typename T>
std::string stringify(const T& v)
{
  if constexpr (std::is_same_v<T, std::nullptr_t>)
    return "nullptr";
  else if constexpr (std::is_enum_v<T>)
    return std::to_string(static_cast<long long>(v));
  else if constexpr (std::is_arithmetic_v<T>)
    return std::to_string(v);
  else if constexpr (std::is_pointer_v<T>)
    return v ? "<non-null ptr>" : "nullptr";
  else
    return "<?>";
}

inline bool approx(float a, float b, float eps = 1e-4f)
{
  return std::fabs(a - b) <= eps;
}

// --- expression decomposition ----------------------------------------------
// CHECK(a == b) becomes (Decomposer{} << a) == b. Because << binds tighter than
// the comparison operators, the left operand is captured first, then the
// comparison records both operands and the result. Compound expressions
// (a && b) collapse to a plain bool and report the source text only.
struct Result
{
  bool passed;
  std::string lhs;
  const char* op;
  std::string rhs;
  explicit operator bool() const { return passed; }
};

template <typename L>
struct ExprLhs
{
  L lhs;

  explicit operator bool() const { return static_cast<bool>(lhs); }

  template <typename R>
  Result operator==(const R& r) const
  {
    return {static_cast<bool>(lhs == r), stringify(lhs), "==", stringify(r)};
  }
  template <typename R>
  Result operator!=(const R& r) const
  {
    return {static_cast<bool>(lhs != r), stringify(lhs), "!=", stringify(r)};
  }
  template <typename R>
  Result operator<(const R& r) const
  {
    return {static_cast<bool>(lhs < r), stringify(lhs), "<", stringify(r)};
  }
  template <typename R>
  Result operator>(const R& r) const
  {
    return {static_cast<bool>(lhs > r), stringify(lhs), ">", stringify(r)};
  }
  template <typename R>
  Result operator<=(const R& r) const
  {
    return {static_cast<bool>(lhs <= r), stringify(lhs), "<=", stringify(r)};
  }
  template <typename R>
  Result operator>=(const R& r) const
  {
    return {static_cast<bool>(lhs >= r), stringify(lhs), ">=", stringify(r)};
  }
};

struct Decomposer
{
  template <typename L>
  ExprLhs<const L&> operator<<(const L& l) const
  {
    return ExprLhs<const L&>{l};
  }
};

inline std::string failDetail(const char* file,
                              int line,
                              const char* expr,
                              const std::string& expected,
                              const std::string& actual)
{
  return std::string("      ") + cFail() + "FAIL" + cReset() + " " + file +
         ":" + std::to_string(line) + ": " + expr + "\n" + "             " +
         cDim() + "Expected:" + cReset() + " " + expected + "\n" +
         "             " + cDim() + "Actual:" + cReset() + "   " + actual +
         "\n";
}

inline void check(const Result& r, const char* expr, const char* file, int line)
{
  if (r.passed)
  {
    ++detail::g_total.passed;
    return;
  }
  ++detail::g_total.failed;
  // For a == the right operand is the expected value; for the other operators
  // it is a bound, so keep the operator with it (e.g. "> 0", "!= 3").
  const std::string expected =
      std::string(r.op) == "==" ? r.rhs : std::string(r.op) + " " + r.rhs;
  detail::emit(failDetail(file, line, expr, expected, r.lhs));
}

template <typename L>
void check(const ExprLhs<L>& e, const char* expr, const char* file, int line)
{
  if (static_cast<bool>(e.lhs))
  {
    ++detail::g_total.passed;
    return;
  }
  ++detail::g_total.failed;
  detail::emit(failDetail(file, line, expr, "true", stringify(e.lhs)));
}

inline void check(bool passed, const char* expr, const char* file, int line)
{
  if (passed)
  {
    ++detail::g_total.passed;
    return;
  }
  ++detail::g_total.failed;
  detail::emit(failDetail(file, line, expr, "true", "false"));
}

// A named test scope. On destruction it prints its pass/fail tally and then the
// failures that occurred inside it (a delta of the running totals), so checks
// made outside any test still count toward the program total.
class TestScope
{
public:
  explicit TestScope(const char* name) : m_name(name), m_start(detail::g_total)
  {
    ++detail::g_testDepth;
  }

  ~TestScope()
  {
    --detail::g_testDepth;
    const int passed = detail::g_total.passed - m_start.passed;
    const int failed = detail::g_total.failed - m_start.failed;
    const bool ok = failed == 0;
    const char* color = ok ? cPass() : cFail();
    std::fprintf(stderr,
                 "  [ %s%s%s ] %-52s %s%d/%d%s\n",
                 color,
                 ok ? "PASS" : "FAIL",
                 cReset(),
                 m_name,
                 color,
                 passed,
                 passed + failed,
                 cReset());
    detail::flushBuffer(); // the test's failures, listed under its header
  }

  explicit operator bool()
  {
    const bool first = !m_done;
    m_done = true;
    return first;
  }

private:
  const char* m_name;
  Stats m_start;
  bool m_done = false;
};

inline int report(const char* program)
{
  detail::flushBuffer(); // any failures from outside a test
  const bool ok = detail::g_total.failed == 0;
  std::fprintf(stderr,
               "%s%s: %d passed, %d failed%s\n",
               ok ? cPass() : cFail(),
               program,
               detail::g_total.passed,
               detail::g_total.failed,
               cReset());
  return ok ? 0 : 1;
}
} // namespace testing

// The decomposition deliberately relies on << binding tighter than the
// comparison, which trips a precedence warning at every call site; silence just
// that warning around the expansion.
#if defined(__clang__)
  #define SFS_CHECK_PUSH                                                       \
    _Pragma("clang diagnostic push") _Pragma(                                  \
        "clang diagnostic ignored \"-Woverloaded-shift-op-parentheses\"")
  #define SFS_CHECK_POP _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
  #define SFS_CHECK_PUSH                                                       \
    _Pragma("GCC diagnostic push")                                             \
        _Pragma("GCC diagnostic ignored \"-Wparentheses\"")
  #define SFS_CHECK_POP _Pragma("GCC diagnostic pop")
#else
  #define SFS_CHECK_PUSH
  #define SFS_CHECK_POP
#endif

// Variadic so a condition containing commas (e.g. view<A, B>()) isn't split
// into multiple macro arguments by the preprocessor.
#define CHECK(...)                                                             \
  do                                                                           \
  {                                                                            \
    SFS_CHECK_PUSH                                                             \
    ::testing::check(::testing::Decomposer{} << __VA_ARGS__,                   \
                     #__VA_ARGS__,                                             \
                     __FILE__,                                                 \
                     __LINE__);                                                \
    SFS_CHECK_POP                                                              \
  } while (false)

// TEST("name") { ... } runs the following block as a named test that reports
// its own tally. A one-shot for-loop, so the block reads naturally and its
// locals stay scoped to it.
#define TEST(name) for (::testing::TestScope _sfs_test{name}; _sfs_test;)
