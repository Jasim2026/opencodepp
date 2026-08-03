// smoke_test.cpp -- Phase 0 trivial harness.
// Verifies the toolchain, the C ABI header (inline fn), and the error taxonomy
// compile AND behave. Real module tests land with each phase.
#include <cstdio>
#include <cstdlib>

#include "opencode/opencode.h"
#include "core/error.h"

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

void test_arithmetic() {
    CHECK(1 + 1 == 2);
}

void test_abi_header() {
    CHECK(opencode_abi_version() == OPENCODE_ABI_VERSION);
    CHECK(OPENCODE_ABI_VERSION >= 1u);
    /* every status value is distinct and valid */
    CHECK(OPENCODE_OK == 0);
    CHECK(OPENCODE_ERR_CANCELLED != OPENCODE_ERR_FATAL);
}

void test_error_taxonomy() {
    using namespace opencode::core;
    /* classification: transient network errors are retryable */
    CHECK(retry_class(Err::e_net_timeout) == Retry::retryable);
    CHECK(retry_class(Err::e_rate_limit) == Retry::retryable);
    CHECK(retry_class(Err::e_net_offline) == Retry::retryable);
    /* deterministic errors are not retried */
    CHECK(retry_class(Err::e_auth) == Retry::none);
    CHECK(retry_class(Err::e_invalid_cfg) == Retry::none);
    /* invariant breakage is fatal, never swallowed */
    CHECK(retry_class(Err::e_internal) == Retry::fatal);
    CHECK(retry_class(Err::e_oom) == Retry::fatal);

    /* ABI mapping is the single source of truth */
    CHECK(to_abi_status(Err::ok) == OPENCODE_OK);
    CHECK(to_abi_status(Err::e_net_offline) == OPENCODE_ERR_NO_NETWORK);
    CHECK(to_abi_status(Err::e_auth) == OPENCODE_ERR_AUTH);
    CHECK(to_abi_status(Err::e_cancelled) == OPENCODE_ERR_CANCELLED);

    /* error_code is small, copyable, allocation-free */
    error_code ec(Err::e_net_timeout, 5);
    CHECK(ec.code() == Err::e_net_timeout);
    CHECK(ec.detail() == 5);
    CHECK(ec.retry() == Retry::retryable);
    error_code copy = ec;
    CHECK(copy == ec);
    CHECK(ec == Err::e_net_timeout);
    CHECK(Err::e_net_timeout == ec);
    CHECK(ok().ok());
    CHECK(!ec.ok());
    CHECK(ec.message() == "net_timeout");
}

} /* namespace */

int main() {
    test_arithmetic();
    test_abi_header();
    test_error_taxonomy();

    if (failures == 0) {
        std::printf("smoke_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "smoke_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
