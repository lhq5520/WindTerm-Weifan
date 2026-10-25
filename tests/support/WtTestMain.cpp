#include "WtTest.h"

#include <cstdio>
#include <cstring>
#include <exception>

namespace wt::test {
namespace {
int g_failures = 0;
}

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void reportFailure(const char* file, int line, const std::string& message) {
    ++g_failures;
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
}

} // namespace wt::test

int main(int argc, char** argv) {
    using namespace wt::test;
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failedTests = 0;
    for (const auto& test : registry()) {
        if (filter && !std::strstr(test.name, filter)) {
            continue;
        }
        ++run;
        const int before = g_failures;
        try {
            test.body();
        } catch (const RequireFailed&) {
        } catch (const std::exception& e) {
            reportFailure(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        }
        const bool ok = g_failures == before;
        failedTests += ok ? 0 : 1;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", test.name);
        std::fflush(stdout);
    }
    std::printf("%d test(s), %d failed\n", run, failedTests);
    if (run == 0) {
        std::fprintf(stderr, "no tests matched\n");
        return 2;
    }
    return failedTests == 0 ? 0 : 1;
}
