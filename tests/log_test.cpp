// log_test.cpp -- Phase 1: logfmt writer, filtering, file sink.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

std::string g_last;
int g_lines = 0;

void sink(void*, opencode::core::Level l, std::string_view line) {
    (void)l;
    g_last.assign(line.data(), line.size());
    ++g_lines;
}

void test_logfmt_line() {
    using namespace opencode::core;
    Logger lg(&sink, nullptr, Level::trace);
    lg.info("hello", "key", "value", "n", int64_t(42), "ok", true);
    CHECK(g_last.find("msg=hello") != std::string::npos);
    CHECK(g_last.find("key=value") != std::string::npos);
    CHECK(g_last.find("n=42") != std::string::npos);
    CHECK(g_last.find("ok=true") != std::string::npos);
}

void test_level_filter() {
    using namespace opencode::core;
    g_lines = 0;
    Logger lg(&sink, nullptr, Level::warn);
    lg.info("should be dropped");
    lg.warn("shown");
    lg.error("also shown");
    CHECK(g_lines == 2);
    CHECK(g_last.find("shown") != std::string::npos);
}

void test_kv_escaping() {
    using namespace opencode::core;
    Logger lg(&sink, nullptr, Level::trace);
    lg.info("m", "weird", "a\"b c");
    CHECK(g_last.find("weird=\"a\\\"b c\"") != std::string::npos);
}

void test_numbers() {
    using namespace opencode::core;
    Logger lg(&sink, nullptr, Level::trace);
    lg.info("m", "u", uint64_t(18446744073709551615ull), "d", 3.5, "f", 0.5);
    CHECK(g_last.find("u=18446744073709551615") != std::string::npos);
    CHECK(g_last.find("d=3.5") != std::string::npos);
}

void test_level_name() {
    using namespace opencode::core;
    CHECK(level_name(Level::trace) == "trace");
    CHECK(level_name(Level::error) == "error");
    CHECK(level_name(Level::info) == "info");
}
} /* namespace */

int main() {
    test_logfmt_line();
    test_level_filter();
    test_kv_escaping();
    test_numbers();
    test_level_name();
    if (failures == 0) {
        std::printf("log_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "log_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
