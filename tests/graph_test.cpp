// graph_test.cpp -- Phase 7: symbol index, call graph, targeted context.
// Gate: lookup accuracy, 1-hop call attribution, snippet caps, lazy
// re-parse on change, LRU eviction, and no-crash on malformed input.
// Runs from the repo root (fixtures at tests/fixtures/workspace/).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "graph/index.h"

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

using namespace opencode::graph;

std::string ws(const std::string& p) { return "tests/fixtures/workspace/" + p; }

bool has_str(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v)
        if (x == s) return true;
    return false;
}
bool has_id(const std::vector<SymId>& v, SymId id) {
    for (const auto x : v)
        if (x == id) return true;
    return false;
}
std::size_t count_named(const std::vector<Sym>& syms, const std::string& name) {
    std::size_t n = 0;
    for (const auto& s : syms)
        if (s.name == name) ++n;
    return n;
}

void write_file(const std::string& path, const std::string& content) {
    /* the scratch dir may not exist on a fresh runner */
    const char* slash = std::strrchr(path.c_str(), '/');
    if (slash != nullptr && slash != path.c_str()) {
        const std::string dir(path.c_str(), static_cast<size_t>(slash - path.c_str()));
        const int rc = ::mkdir(dir.c_str(), 0755);
        (void)rc; /* EEXIST is fine */
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

void test_detect_lang() {
    SymbolIndex idx;
    CHECK(idx.detect_lang("a.c") == Lang::c);
    CHECK(idx.detect_lang("a.cpp") == Lang::cpp);
    CHECK(idx.detect_lang("a.hpp") == Lang::cpp);
    CHECK(idx.detect_lang("a.h") == Lang::cpp);
    CHECK(idx.detect_lang("a.go") == Lang::go);
    CHECK(idx.detect_lang("a.txt") == Lang::unknown);
    CHECK(idx.detect_lang("README") == Lang::unknown);
}

void test_c_index_and_callgraph() {
    SymbolIndex idx;
    CHECK(idx.ensure_indexed(ws("math.c")).ok());
    CHECK(idx.file_count() == 1);
    CHECK(idx.dep_count() >= 3); /* include math.h + add + twice calls */

    Sym add, twice, apply;
    CHECK(idx.lookup("add", ws("math.c"), add).ok());
    CHECK(add.kind == SymKind::function);
    CHECK(idx.lookup("twice", ws("math.c"), twice).ok());
    CHECK(idx.lookup("apply", ws("math.c"), apply).ok());

    /* 1-hop call graph */
    std::vector<std::string> cals = idx.callees(apply.id);
    CHECK(has_str(cals, "twice"));
    cals = idx.callees(twice.id);
    CHECK(has_str(cals, "add"));
    std::vector<SymId> callers = idx.callers_of_name("add");
    CHECK(has_id(callers, twice.id));
    CHECK(has_id(idx.callers_of(add.id), twice.id));

    /* all() by kind */
    std::vector<Sym> funcs = idx.all(SymKind::function, "", 0);
    CHECK(funcs.size() >= 3);
    CHECK(count_named(funcs, "apply") == 1);
    CHECK(idx.all(SymKind::function, "tw", 10).size() == 1);
}

void test_cpp_index() {
    SymbolIndex idx;
    CHECK(idx.ensure_indexed(ws("util.cpp")).ok());
    CHECK(idx.ensure_indexed(ws("util.h")).ok());

    Sym triple, ctor, addm, totalm;
    CHECK(idx.lookup("triple", ws("util.cpp"), triple).ok());
    CHECK(triple.kind == SymKind::function);
    CHECK(idx.lookup("Accumulator::add", ws("util.cpp"), addm).ok());
    CHECK(addm.name == "Accumulator::add");
    CHECK(idx.lookup("Accumulator::total", ws("util.cpp"), totalm).ok());
    CHECK(idx.lookup("Accumulator::Accumulator", ws("util.cpp"), ctor).ok());

    Sym acc;
    CHECK(idx.lookup("Accumulator", ws("util.h"), acc).ok());
    CHECK(acc.kind == SymKind::class_);

    /* method body calls triple -> same-file to_sym resolution */
    CHECK(has_str(idx.callees(addm.id), "triple"));
    CHECK(has_id(idx.callers_of(triple.id), addm.id));
}

void test_go_index() {
    SymbolIndex idx;
    CHECK(idx.ensure_indexed(ws("main.go")).ok());
    CHECK(idx.dep_count() >= 6); /* fmt + strings imports + calls */

    Sym pkg, point, magn, total, main;
    CHECK(idx.lookup("main", ws("main.go"), main).ok());
    CHECK(main.kind == SymKind::function);
    CHECK(idx.lookup("Point", ws("main.go"), point).ok());
    CHECK(point.kind == SymKind::type);
    CHECK(idx.lookup("magnitude", ws("main.go"), magn).ok());
    CHECK(magn.kind == SymKind::method);
    CHECK(magn.vis == Visibility::private_);
    CHECK(idx.lookup("total", ws("main.go"), total).ok());

    /* main() calls five things; total() is called from main() */
    std::vector<std::string> cals = idx.callees(main.id);
    CHECK(has_str(cals, "total"));
    CHECK(has_str(cals, "magnitude"));
    CHECK(has_str(cals, "scale"));
    CHECK(has_str(cals, "Println"));
    CHECK(has_str(cals, "ToUpper"));
    CHECK(has_id(idx.callers_of_name("total"), main.id));
    CHECK(idx.callees(total.id).empty());

    std::vector<Sym> types = idx.all(SymKind::type, "", 0);
    CHECK(count_named(types, "Point") == 1);
}

void test_lazy_incremental() {
    const std::string path = "/tmp/opencode/graph_lazy.c";
    write_file(path, "int alpha() { return 1; }\n");
    SymbolIndex idx;
    CHECK(idx.ensure_indexed(path).ok());
    const std::uint32_t v0 = idx.version();
    const std::uint32_t e0 = idx.extract_count();

    std::string changed;
    CHECK(idx.ensure_indexed(path, &changed).ok());
    CHECK(changed.empty()); /* unchanged -> no re-parse */
    CHECK(idx.version() == v0);
    CHECK(idx.extract_count() == e0);

    /* modify: new content, different size */
    write_file(path, "int alpha() { return 1; }\nint beta() { return 2; }\n");
    CHECK(idx.ensure_indexed(path, &changed).ok());
    CHECK(changed == path);
    CHECK(idx.version() > v0);
    CHECK(idx.extract_count() == e0 + 1);

    Sym beta;
    CHECK(idx.lookup("beta", path, beta).ok());
    CHECK(beta.kind == SymKind::function);

    std::remove(path.c_str());
}

void test_snippet_and_caps() {
    SymbolIndex idx;
    CHECK(idx.ensure_indexed(ws("math.c")).ok());
    Sym apply;
    CHECK(idx.lookup("apply", ws("math.c"), apply).ok());

    Snippet sn;
    CHECK(idx.snippet(apply.id, 0, sn).ok());
    CHECK(sn.text.find("int apply") != std::string::npos);
    CHECK(sn.line == apply.line);
    CHECK(sn.file == ws("math.c"));

    /* explicit max_bytes cap truncates */
    Snippet small;
    CHECK(idx.snippet(apply.id, 24, small).ok());
    CHECK(small.truncated);
    CHECK(small.bytes <= 24);

    /* unknown id / evicted sym */
    CHECK(!idx.snippet(99999, 0, sn).ok());
}

void test_snippet_for_call() {
    SymbolIndex idx;
    CHECK(idx.ensure_indexed(ws("main.go")).ok());
    CHECK(idx.ensure_indexed(ws("math.c")).ok());

    std::vector<Snippet> out;
    CHECK(idx.snippet_for_call(ws("main.go"), "total", out).ok());
    CHECK(out.size() == 1);
    CHECK(out[0].sym == "total");
    CHECK(out[0].text.find("func total") != std::string::npos);

    /* apply -> [twice]: callee + one small level of callees */
    out.clear();
    CHECK(idx.snippet_for_call(ws("math.c"), "apply", out).ok());
    CHECK(out.size() == 2);
    CHECK(out[0].sym == "apply");
    CHECK(out[1].sym == "twice");

    out.clear();
    CHECK(!idx.snippet_for_call(ws("main.go"), "no_such_fn", out).ok());
}

void test_context_for_change() {
    SymbolIndex idx;
    CHECK(idx.ensure_indexed(ws("main.go")).ok());
    std::vector<std::string> files{ws("main.go")};
    std::vector<Snippet> out;
    CHECK(idx.context_for_change(files, out).ok());
    CHECK(!out.empty());
    std::size_t total = 0;
    for (const auto& s : out) {
        CHECK(s.bytes == s.text.size());
        total += s.bytes;
    }
    CHECK(total <= 16384); /* IndexLimits.context_total_bytes */
    bool found_main = false;
    for (const auto& s : out)
        if (s.sym == "main") found_main = true;
    CHECK(found_main);

    out.clear();
    CHECK(idx.context_for_change({"tests/fixtures/workspace/missing.cpp"}, out)
              .ok());
    CHECK(out.empty());
}

void test_lru_eviction() {
    SymbolIndex idx;
    idx.set_cache_cap(2);
    CHECK(idx.ensure_indexed(ws("math.c")).ok());
    CHECK(idx.ensure_indexed(ws("util.cpp")).ok());
    CHECK(idx.ensure_indexed(ws("main.go")).ok());
    CHECK(idx.file_count() <= 2); /* oldest (math.c) evicted */

    Sym gone;
    CHECK(!idx.lookup("apply", ws("math.c"), gone).ok());

    /* re-indexing evicted file re-parses */
    const std::uint32_t e0 = idx.extract_count();
    CHECK(idx.ensure_indexed(ws("math.c")).ok());
    CHECK(idx.extract_count() == e0 + 1);
    CHECK(idx.lookup("apply", ws("math.c"), gone).ok());
    CHECK(gone.kind == SymKind::function);

    /* eviction is LRU: util.cpp (now oldest) dropped, main.go survives */
    Sym point;
    CHECK(idx.lookup("Point", ws("main.go"), point).ok());
    Sym triple;
    CHECK(!idx.lookup("triple", ws("util.cpp"), triple).ok());
}

void test_malformed_no_crash() {
    const std::string path = "/tmp/opencode/graph_bad.c";
    write_file(path,
               "int foo( { if ( { ) ;\n"
               "char* s = \"unterminated\n"
               "int bar() { return ;\n"
               "void ( x ) { }\n");
    SymbolIndex idx;
    CHECK(idx.ensure_indexed(path).ok()); /* no exception, no crash */
    Sym foo;
    idx.lookup("foo", path, foo); /* must not throw either way */
    std::remove(path.c_str());
}

} /* namespace */

int main() {
    std::printf("graph_test\n");
    test_detect_lang();
    test_c_index_and_callgraph();
    test_cpp_index();
    test_go_index();
    test_lazy_incremental();
    test_snippet_and_caps();
    test_snippet_for_call();
    test_context_for_change();
    test_lru_eviction();
    test_malformed_no_crash();
    if (failures == 0) {
        std::printf("graph_test: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::printf("graph_test: %d FAILURE(s)\n", failures);
    return EXIT_FAILURE;
}
