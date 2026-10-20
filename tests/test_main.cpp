#include "testing.hpp"

#include <chrono>
#include <cstring>
#include <iostream>

// Usage: ipbrs_tests [substring]   runs tests whose name contains substring.
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "";
    int passed = 0, failed = 0;
    for (const testing::TestCase& test : testing::registry()) {
        if (std::strstr(test.name, filter) == nullptr) continue;
        const auto start = std::chrono::steady_clock::now();
        try {
            test.body();
            ++passed;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "[ PASS ] " << test.name << " (" << static_cast<int>(ms) << " ms)\n";
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[ FAIL ] " << test.name << "\n         " << error.what() << "\n";
        }
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 && passed > 0 ? 0 : 1;
}
