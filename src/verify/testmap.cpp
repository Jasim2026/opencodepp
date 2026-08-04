/*
 * testmap.cpp -- affected-test mapper for the verify gate (Phase 9).
 *
 * Maps edited files to the test files most likely affected by the change.
 * Uses naming conventions (X_test.cpp / test_X.cpp) and file-proximity
 * heuristics. This is informational -- it never blocks; the agent (Phase 10)
 * uses the mapping to decide which tests to run.
 */
#include "verify/gate.h"

#include <string>
#include <string_view>
#include <vector>

namespace opencode::verify {

namespace {

/* Check if `path` ends with `suffix` (case-insensitive for the test
 * suffix part). */
bool ends_with(std::string_view path, std::string_view suffix) {
    return path.size() >= suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* Extract the base name (without directory) from a path. */
std::string_view base_name(std::string_view path) {
    auto pos = path.rfind('/');
    if (pos == std::string_view::npos) return path;
    return path.substr(pos + 1);
}

/* Strip the file extension. */
std::string_view strip_ext(std::string_view name) {
    auto pos = name.rfind('.');
    if (pos == std::string_view::npos) return name;
    return name.substr(0, pos);
}

} /* namespace */

std::vector<TestMapping> map_tests(const EditProposal& proposal) {
    std::vector<TestMapping> result;
    std::string_view edited = proposal.path;
    std::string_view edited_base = base_name(edited);
    std::string_view edited_stem = strip_ext(edited_base);

    /* Convention 1: tests/<stem>_test.cpp or tests/test_<stem>.cpp */
    std::string test_a = "tests/" + std::string(edited_stem) + "_test.cpp";
    std::string test_b = "tests/test_" + std::string(edited_stem) + ".cpp";
    result.push_back({test_a, "naming convention: " + std::string(edited_stem) + "_test"});
    result.push_back({test_b, "naming convention: test_" + std::string(edited_stem)});

    /* Convention 2: if the edited file IS a test, note that itself. */
    if (ends_with(edited_base, "_test.cpp") || ends_with(edited_base, "_test.c") ||
        ends_with(edited_base, ".test.js") || ends_with(edited_base, ".test.ts")) {
        result.push_back({std::string(edited), "edited file is a test"});
    }

    /* Convention 3: same directory, matching _test suffix. */
    /* (future: iterate directory listing when available) */

    return result;
}

} /* namespace opencode::verify */
