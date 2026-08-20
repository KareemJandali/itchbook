#pragma once
//
// A 20-line test harness. Deliberately not GoogleTest: the build stays
// dependency-free and hermetic, which matters more right now than the extra
// matchers would. Revisit when property-based fuzzing lands in phase 5.
//
// Exit code 0 = all passed, 1 = at least one failure.
//
#include <cstdio>
#include <string>

namespace itchbook::test {

inline int failures = 0;

}  // namespace itchbook::test

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++itchbook::test::failures;                                        \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        auto lhs_ = (a);                                                       \
        auto rhs_ = (b);                                                       \
        if (!(lhs_ == rhs_)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d  %s == %s  (%lld vs %lld)\n",     \
                         __FILE__, __LINE__, #a, #b,                           \
                         static_cast<long long>(lhs_),                         \
                         static_cast<long long>(rhs_));                        \
            ++itchbook::test::failures;                                        \
        }                                                                      \
    } while (0)

// CHECK_EQ casts both sides to long long to print them, which is fine for the
// integers that make up most of this repo's assertions and impossible for a
// string. Where the thing under test IS a sequence — the exact stream of
// messages a receiver delivered, say — seeing both sides on failure is the
// whole value of the assertion, so it gets its own macro rather than being
// reduced to a bare CHECK that prints nothing useful.
#define CHECK_STR(a, b)                                                        \
    do {                                                                       \
        const std::string lhs_ = (a);                                          \
        const std::string rhs_ = (b);                                          \
        if (lhs_ != rhs_) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s == %s\n    got      \"%s\"\n"  \
                                 "    expected \"%s\"\n",                       \
                         __FILE__, __LINE__, #a, #b, lhs_.c_str(),             \
                         rhs_.c_str());                                        \
            ++itchbook::test::failures;                                        \
        }                                                                      \
    } while (0)

#define REPORT()                                                               \
    (itchbook::test::failures == 0                                             \
         ? (std::printf("%s: all checks passed\n", __FILE__), 0)               \
         : (std::fprintf(stderr, "%s: %d failure(s)\n", __FILE__,              \
                         itchbook::test::failures),                            \
            1))
