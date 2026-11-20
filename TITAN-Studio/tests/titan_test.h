// TITAN minimal test harness (no third-party dependency). Original TITAN code.
#pragma once

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace titan_test {
struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
struct Register {
    Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
struct Failure : std::exception {
    std::string msg;
    explicit Failure(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};
inline int runAll() {
    int failed = 0;
    for (auto& c : registry()) {
        try {
            c.fn();
            std::printf("[ PASS ] %s\n", c.name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[ FAIL ] %s\n         %s\n", c.name, e.what());
        }
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failed);
    return failed == 0 ? 0 : 1;
}
}  // namespace titan_test

#define TT_CAT2(a, b) a##b
#define TT_CAT(a, b) TT_CAT2(a, b)
#define TEST(name)                                                                       \
    static void TT_CAT(test_fn_, __LINE__)();                                            \
    static titan_test::Register TT_CAT(test_reg_, __LINE__)(name, TT_CAT(test_fn_, __LINE__)); \
    static void TT_CAT(test_fn_, __LINE__)()
#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        if (!(cond))                                                                                  \
            throw titan_test::Failure(std::string("CHECK(" #cond ") failed at ") + __FILE__ + ":" +   \
                                      std::to_string(__LINE__));                                      \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) <= (eps))
#define CHECK_THROWS(expr)                         \
    do {                                           \
        bool thrown_ = false;                      \
        try {                                      \
            (void)(expr);                          \
        } catch (...) {                            \
            thrown_ = true;                        \
        }                                          \
        CHECK(thrown_ && "expected exception: " #expr); \
    } while (0)
#define TITAN_TEST_MAIN() \
    int main() { return titan_test::runAll(); }
