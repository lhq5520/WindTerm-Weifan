#pragma once

// Minimal dependency-free test harness for the Qt-free libraries.
//
//   WT_TEST(name) { WT_CHECK(cond); WT_CHECK_EQ(a, b); WT_REQUIRE(cond); }
//
// Each test executable links tests/support/WtTestMain.cpp. Pass a substring as argv[1] to run a
// subset of tests.

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace wt::test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& registry();
void reportFailure(const char* file, int line, const std::string& message);

struct RequireFailed {};

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_same_v<T, std::string> || std::is_convertible_v<T, const char*>) {
        std::string escaped = "\"";
        for (unsigned char c : std::string(value)) {
            if (c < 0x20 || c == 0x7F) {
                static const char* hex = "0123456789abcdef";
                escaped += "\\x";
                escaped += hex[c >> 4];
                escaped += hex[c & 0xF];
            } else {
                escaped += static_cast<char>(c);
            }
        }
        return escaped + "\"";
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        return describe(std::string(value));
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (requires(std::ostream& os, const T& v) { os << v; }) {
        std::ostringstream os;
        os << value;
        return os.str();
    } else {
        return "<value>";
    }
}

} // namespace wt::test

#define WT_CONCAT_INNER(a, b) a##b
#define WT_CONCAT(a, b) WT_CONCAT_INNER(a, b)

#define WT_TEST(name)                                                                                    \
    static void WT_CONCAT(wtTest_, name)();                                                              \
    static const ::wt::test::Registrar WT_CONCAT(wtRegistrar_, name)(#name, &WT_CONCAT(wtTest_, name)); \
    static void WT_CONCAT(wtTest_, name)()

#define WT_CHECK(cond)                                                                                   \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            ::wt::test::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")");                         \
        }                                                                                                \
    } while (0)

#define WT_REQUIRE(cond)                                                                                 \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            ::wt::test::reportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");                       \
            throw ::wt::test::RequireFailed{};                                                           \
        }                                                                                                \
    } while (0)

#define WT_CHECK_EQ(actual, expected)                                                                    \
    do {                                                                                                 \
        const auto wtActual = (actual); /* copies: operands may be temporaries */                        \
        const auto wtExpected = (expected);                                                              \
        if (!(wtActual == wtExpected)) {                                                                 \
            ::wt::test::reportFailure(__FILE__, __LINE__,                                                \
                                      "CHECK_EQ(" #actual ", " #expected ")\n      actual:   " +          \
                                          ::wt::test::describe(wtActual) +                               \
                                          "\n      expected: " + ::wt::test::describe(wtExpected));      \
        }                                                                                                \
    } while (0)
