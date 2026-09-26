#pragma once

// Minimal test framework: TEST(name) { CHECK(cond); CHECK_EQ(a, b); }

#include <cstdio>
#include <functional>
#include <vector>

namespace test {

struct Case {
  const char* name;
  std::function<void()> fn;
};
inline std::vector<Case>& cases() {
  static std::vector<Case> all;
  return all;
}
inline int& failures() {
  static int n = 0;
  return n;
}
struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};

}  // namespace test

#define TEST(name)                                                  \
  static void name();                                               \
  static test::Registrar registrar_##name(#name, name);             \
  static void name()

#define CHECK(cond)                                                               \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++test::failures();                                                         \
    }                                                                             \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    auto va_ = (a);                                                                                 \
    auto vb_ = (b);                                                                                 \
    if (!(va_ == vb_)) {                                                                            \
      std::fprintf(stderr, "  %s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, \
                   (long long)va_, #b, (long long)vb_);                                             \
      ++test::failures();                                                                           \
    }                                                                                               \
  } while (0)
