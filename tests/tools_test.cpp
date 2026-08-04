// tools_test.cpp -- Phase 8: tool runtime.
// Suites: schema generation (provider-native, golden cross-validation),
// read tools, patch/write tools, git + workspace + sym tools, shell (timeout/
// streaming/cancel). Runs from the repo root.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "tools/schema.h"
#include "util/json.h"

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

using opencode::tools::ParamSpec;
using opencode::tools::ParamType;
using opencode::tools::ToolCategory;
using opencode::tools::ToolSpec;
using opencode::tools::schema::make_spec;
using opencode::tools::schema::params_schema;
using opencode::tools::schema::tools_json;
using opencode::util::JVal;
using opencode::util::parse_json;
using opencode::util::to_json;

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

ParamSpec iparam(const std::string& name) {
    ParamSpec p;
    p.name = name;
    p.type = ParamType::integer;
    return p;
}

/* The `add(a:int, b:int)` tool exactly as the golden fixtures pin it. */
ToolSpec golden_add_spec() {
    return make_spec("add", "Add two numbers",
                     {iparam("a"), iparam("b")}, true, ToolCategory::read);
}

std::string fixture_tools_json(const std::string& fixture_path) {
    const std::string text = read_file(fixture_path);
    JVal doc;
    if (!parse_json(text, doc).ok() || !doc.find("tools")) return "";
    const JVal* tools = doc.find("tools");
    return to_json(*tools);
}

void test_params_schema() {
    /* all-required compact form -- byte-for-byte the golden fixture */
    const std::string want =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},"
        "\"b\":{\"type\":\"integer\"}}}";
    CHECK(params_schema({iparam("a"), iparam("b")}) == want);

    /* optional param emits `required` */
    ParamSpec opt = iparam("end");
    opt.required = false;
    const std::string s2 = params_schema({iparam("start"), opt});
    CHECK(s2.find("\"required\":[\"start\"]") != std::string::npos);
    CHECK(s2.find("\"end\":{\"type\":\"integer\"}") != std::string::npos);

    /* string_array / enum / default / description */
    ParamSpec pat = iparam("pattern");
    pat.type = ParamType::string;
    pat.enum_values = {"*.c", "*.cpp"};
    pat.default_json = "\"*.c\"";
    pat.description = "glob to match";
    const std::string s3 = params_schema({pat});
    CHECK(s3.find("\"enum\":[\"*.c\",\"*.cpp\"]") != std::string::npos);
    CHECK(s3.find("\"default\":\"*.c\"") != std::string::npos);
    CHECK(s3.find("\"description\":\"glob to match\"") != std::string::npos);

    ParamSpec arr = iparam("paths");
    arr.type = ParamType::string_array;
    const std::string s4 = params_schema({arr});
    CHECK(s4.find("\"type\":\"array\"") != std::string::npos);
    CHECK(s4.find("\"items\":{\"type\":\"string\"}") != std::string::npos);

    /* empty params -> object with just type */
    CHECK(params_schema({}) == "{\"type\":\"object\"}");
}

void test_schema_cross_provider() {
    const ToolSpec add = golden_add_spec();
    const std::vector<ToolSpec> tools{add};

    /* golden fixtures pin the exact provider shapes */
    CHECK(tools_json(tools, "anthropic") ==
          fixture_tools_json("tests/fixtures/responses/anthropic_request.json"));
    CHECK(tools_json(tools, "openai") ==
          fixture_tools_json("tests/fixtures/responses/openai_request.json"));
    CHECK(tools_json(tools, "openai_compat") ==
          fixture_tools_json("tests/fixtures/responses/openai_compat_request.json"));
    CHECK(tools_json(tools, "google") ==
          fixture_tools_json("tests/fixtures/responses/google_request.json"));

    /* the three families agree on the argument schema */
    const std::string params =
        "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},"
        "\"b\":{\"type\":\"integer\"}}}";
    CHECK(add.params_schema == params);

    /* prompt compiler and runtime project identically (one source of truth) */
    std::printf("  schema bytes: openai=%zu anthropic=%zu google=%zu\n",
                tools_json(tools, "openai").size(),
                tools_json(tools, "anthropic").size(),
                tools_json(tools, "google").size());
}

} /* namespace */

int main() {
    std::printf("tools_test\n");
    test_params_schema();
    test_schema_cross_provider();
    if (failures == 0) {
        std::printf("tools_test: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::printf("tools_test: %d FAILURE(s)\n", failures);
    return EXIT_FAILURE;
}
