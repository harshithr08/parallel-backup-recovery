#pragma once

// Minimal self-registering test harness (no external dependencies).

#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void fail(const char* file, int line, const std::string& message) {
    std::ostringstream out;
    out << file << ":" << line << ": " << message;
    throw Failure(out.str());
}

} // namespace testing

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                                       \
    static void TEST_CAT(test_body_, name)();                                            \
    static ::testing::Registrar TEST_CAT(test_registrar_, name)(#name, &TEST_CAT(test_body_, name)); \
    static void TEST_CAT(test_body_, name)()

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) ::testing::fail(__FILE__, __LINE__, "CHECK failed: " #condition); \
    } while (0)

#define CHECK_EQ(actual, expected)                                                       \
    do {                                                                                 \
        const auto& check_actual_ = (actual);                                            \
        const auto& check_expected_ = (expected);                                        \
        if (!(check_actual_ == check_expected_)) {                                       \
            std::ostringstream check_out_;                                               \
            check_out_ << "CHECK_EQ failed: " #actual " == " #expected " (got " << check_actual_ \
                       << ", expected " << check_expected_ << ")";                       \
            ::testing::fail(__FILE__, __LINE__, check_out_.str());                       \
        }                                                                                \
    } while (0)

#define CHECK_THROWS(expression)                                                         \
    do {                                                                                 \
        bool check_threw_ = false;                                                       \
        try { (void)(expression); } catch (const std::exception&) { check_threw_ = true; } \
        if (!check_threw_) ::testing::fail(__FILE__, __LINE__, "expected exception: " #expression); \
    } while (0)
