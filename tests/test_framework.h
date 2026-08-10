// A minimal test framework.
//
// The component's core is deliberately free of ESPHome headers so it can be
// compiled and tested on the host (see README). Vendoring doctest or Catch2
// for that would be a lot of third-party code to carry for what amounts to
// "run these functions and count the failures", so this is hand-rolled and
// dependency-free.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace vatest {

struct TestCase {
  const char *name;
  void (*fn)();
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int &failures() {
  static int count = 0;
  return count;
}

inline const char *&current_test() {
  static const char *name = "";
  return name;
}

struct Registrar {
  Registrar(const char *name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char *file, int line, const std::string &detail) {
  failures()++;
  std::printf("  FAIL %s\n       %s:%d\n       %s\n", current_test(), file, line, detail.c_str());
}

// Renders a value for a failure message. Strings get quoted so that trailing
// spaces are visible -- the display protocol pads every field with them, and
// "blank" versus "zero" is a distinction this codebase cares about a lot.
inline std::string show(const std::string &v) { return "\"" + v + "\""; }
inline std::string show(const char *v) { return show(std::string(v)); }
inline std::string show(bool v) { return v ? "true" : "false"; }
template<typename T> std::string show(const T &v) { return std::to_string(v); }

inline int run_all() {
  for (const auto &test : registry()) {
    current_test() = test.name;
    test.fn();
  }
  const int failed = failures();
  std::printf("%s: %d test case%s, %d failure%s\n", failed ? "FAILED" : "PASSED",
              static_cast<int>(registry().size()), registry().size() == 1 ? "" : "s", failed,
              failed == 1 ? "" : "s");
  return failed == 0 ? 0 : 1;
}

}  // namespace vatest

#define TEST_CASE(name)                                    \
  static void name();                                      \
  static ::vatest::Registrar registrar_##name(#name, name); \
  static void name()

#define CHECK(expr)                                                       \
  do {                                                                    \
    if (!(expr))                                                          \
      ::vatest::report_failure(__FILE__, __LINE__, "expected: " #expr);   \
  } while (0)

#define CHECK_EQ(actual, expected)                                              \
  do {                                                                          \
    const auto va_actual = (actual);                                            \
    const auto va_expected = (expected);                                        \
    if (!(va_actual == va_expected))                                            \
      ::vatest::report_failure(__FILE__, __LINE__,                              \
                               #actual " == " #expected "\n       got:      " + \
                                   ::vatest::show(va_actual) +                  \
                                   "\n       expected: " + ::vatest::show(va_expected)); \
  } while (0)
