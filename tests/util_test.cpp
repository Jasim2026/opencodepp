// util_test.cpp -- Phase 1: string, path, base64, hash, JSON.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "util/string.h"
#include "util/path.h"
#include "util/base64.h"
#include "util/hash.h"
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

using namespace opencode::util;

void test_string() {
    CHECK(trim("  hello \n") == "hello");
    CHECK(starts_with("hello world", "hello"));
    CHECK(!starts_with("hello", "hello world"));
    CHECK(ends_with("a/b.c", ".c"));
    CHECK(contains("abcdef", "cd"));
    CHECK(to_lower("AbC") == "abc");
    size_t n = 0;
    split("a,b,,c", ",", [&](std::string_view) { ++n; });
    CHECK(n == 3);
    uint64_t u = 0;
    int64_t i = 0;
    CHECK(parse_u64("12345", u) && u == 12345);
    CHECK(!parse_u64("-1", u));
    CHECK(!parse_u64("abc", u));
    CHECK(parse_i64("-42", i) && i == -42);
    CHECK(parse_i64("9223372036854775807", i) && i == INT64_MAX);
    CHECK(!parse_i64("9223372036854775808", i));
    CHECK(parse_i64("-9223372036854775808", i) && i == INT64_MIN);
}

void test_path() {
    using opencode::util::basename;
    using opencode::util::dirname;
    using opencode::util::extension;
    CHECK(basename("/a/b.c") == "b.c");
    CHECK(basename("b.c") == "b.c");
    CHECK(dirname("/a/b.c") == "/a");
    CHECK(dirname("b.c") == "");
    CHECK(extension("/a/b.c") == "c");
    CHECK(extension("/a/b") == "");
    CHECK(extension(".hidden") == "");
    CHECK(join("a/b", "c") == "a/b/c");
    CHECK(join("a/b", "") == "a/b");
    CHECK(join("", "c") == "c");
    CHECK(is_absolute("/x"));
    CHECK(!is_absolute("x"));
}

void test_base64() {
    const char* msg = "Hello, World!";
    const size_t n = std::strlen(msg);
    char enc[64];
    size_t el = b64_encode(msg, n, enc);
    CHECK(std::string(enc, el) == "SGVsbG8sIFdvcmxkIQ==");

    char dec[64];
    size_t dl = 0;
    CHECK(b64_decode(std::string_view(enc, el), dec, sizeof dec, dl).ok());
    CHECK(dl == n);
    CHECK(std::memcmp(dec, msg, n) == 0);

    size_t empty = 0;
    CHECK(b64_decode("", dec, sizeof dec, empty).ok());
    CHECK(empty == 0);

    /* bad alphabet -> proto_parse */
    char out[16];
    size_t o = 0;
    CHECK(b64_decode("ab*c", out, sizeof out, o).code() ==
          opencode::core::Err::e_proto_parse);
    /* encoded size overflows a tiny buffer -> overflow */
    CHECK(b64_decode("SGVsbG8sIFdvcmxkIQ==", out, 4, o).code() ==
          opencode::core::Err::e_overflow);
}

void test_hash() {
    CHECK(fnv1a64("") == 1469598103934665603ull); /* FNV-1a empty hash */
    uint64_t h1 = fnv1a64("hello");
    CHECK(h1 != fnv1a64("hellO"));
    CHECK(fnv1a64("hello") == h1); /* deterministic */
    CHECK(mix64(1, 2) == mix64(1, 2));
    CHECK(mix64(1, 2) != mix64(2, 1));
}

void test_json_parse() {
    const char* doc = R"({"name":"OpenCode++","ver":7,"ok":true,"none":null,"list":[1,2,3],"nested":{"a":"b\"","emoji":"\u4e2d"}})";
    JVal root;
    size_t pos = 0;
    CHECK(parse_json(doc, root, &pos).ok());
    CHECK(root.kind == JVal::Kind::object);
    const JVal* name = root.find("name");
    CHECK(name && name->kind == JVal::Kind::string && name->str == "OpenCode++");
    const JVal* ver = root.find("ver");
    CHECK(ver && ver->kind == JVal::Kind::number && ver->num == 7);
    const JVal* ok = root.find("ok");
    CHECK(ok && ok->kind == JVal::Kind::boolean && ok->b);
    const JVal* none = root.find("none");
    CHECK(none && none->kind == JVal::Kind::null);
    const JVal* list = root.find("list");
    CHECK(list && list->kind == JVal::Kind::array && list->arr.size() == 3);
    CHECK(list->arr[2].num == 3);
    const JVal* nested = root.find("nested");
    CHECK(nested && nested->kind == JVal::Kind::object);
    const JVal* a = nested->find("a");
    CHECK(a && a->kind == JVal::Kind::string && a->str == "b\"");
    const JVal* emoji = nested->find("emoji");
    CHECK(emoji && emoji->str == "\xe4\xb8\xad"); /* U+4E2D */
    CHECK(root.find("missing") == nullptr);
}

void test_json_errors() {
    JVal v;
    size_t pos = 0;
    CHECK(!parse_json("{\"a\":}", v, &pos).ok());
    CHECK(!parse_json("[1,2", v, &pos).ok());
    CHECK(!parse_json("tru", v, &pos).ok());
    CHECK(!parse_json("\"unterminated", v, &pos).ok());
    CHECK(!parse_json("{\"a\":1} trailing", v, &pos).ok());
    CHECK(!parse_json("", v, &pos).ok());
}

void test_json_roundtrip() {
    JVal root = JVal::Object({{"x", JVal::Num(1.5)}, {"s", JVal::Str("he\"llo")},
                              {"b", JVal::Bool(false)},
                              {"arr", JVal::Array({JVal::Num(1), JVal::Null()})}});
    std::string out = to_json(root);
    JVal back;
    CHECK(parse_json(out, back, nullptr).ok());
    CHECK(back.find("x") && back.find("x")->num == 1.5);
    CHECK(back.find("s") && back.find("s")->str == "he\"llo");
    CHECK(back.find("b") && back.find("b")->kind == JVal::Kind::boolean &&
          !back.find("b")->b);
}
} /* namespace */

int main() {
    test_string();
    test_path();
    test_base64();
    test_hash();
    test_json_parse();
    test_json_errors();
    test_json_roundtrip();
    if (failures == 0) {
        std::printf("util_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "util_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
