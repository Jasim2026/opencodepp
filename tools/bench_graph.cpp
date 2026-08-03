// bench_graph.cpp -- Phase 7: index build time + RSS for a synthetic corpus
// (feeds the graph's "build time + RSS" acceptance metric).
//
// Generates N synthetic translation units (default 200) in /tmp, indexes them
// with the lazy path, then runs a few lookups, printing wall time, extract
// count, sym/dep/file counts and peak RSS. No arguments needed:
//   ./bench_graph            # default corpus
//   ./bench_graph 500        # bigger corpus
#define _POSIX_C_SOURCE 200809L

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "core/error.h"
#include "graph/index.h"

namespace {

using namespace opencode::core;
using namespace opencode::graph;

/* Synthetic single-file C corpus: kFns functions, each calling ~kCalls others,
 * plus a couple of globals and one struct. */
void gen_unit(std::string& out, int n, int kFns, int kCalls) {
    out += "typedef unsigned long size_t;\n";
    out += "struct Entry { int key; int val; };\n";
    for (int i = 0; i < kFns; ++i) {
        out += "static int fn_";
        out += std::to_string(n * kFns + i);
        out += "(int x) {\n";
        for (int c = 0; c < kCalls; ++c) {
            out += "    return fn_";
            out += std::to_string(n * kFns + ((i + c * 3 + 1) % kFns));
            out += "(x + ";
            out += std::to_string(c);
            out += ");\n";
        }
        out += "    return x;\n}\n";
    }
    out += "int global_";
    out += std::to_string(n);
    out += " = 7;\n";
}

std::size_t rss_kb() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0;
    if (std::fscanf(f, "%*s %ld", &pages) != 1) pages = 0;
    std::fclose(f);
    return static_cast<std::size_t>(pages) * 4; /* 4 KiB pages */
}

} /* namespace */

int main(int argc, char** argv) {
    const int kFiles = argc > 1 ? std::atoi(argv[1]) : 200;
    const int kFns = 12;
    const int kCalls = 4;

    const std::string dir = "/tmp/opencode/graph_bench";
    const int r_clean = std::system(("rm -rf " + dir).c_str());
    (void)r_clean;
    const int r1 = std::system(("mkdir -p " + dir).c_str());
    (void)r1;

    std::vector<std::string> paths;
    for (int n = 0; n < kFiles; ++n) {
        std::string src;
        gen_unit(src, n, kFns, kCalls);
        const std::string path = dir + "/unit_" + std::to_string(n) + ".c";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return EXIT_FAILURE;
        std::fwrite(src.data(), 1, src.size(), f);
        std::fclose(f);
        paths.push_back(path);
    }

    SymbolIndex idx;
    const auto t0 = std::chrono::steady_clock::now();
    for (const std::string& p : paths) {
        const error_code ec = idx.ensure_indexed(p);
        if (!ec.ok()) {
            std::fprintf(stderr, "ensure_indexed(%s) failed\n", p.c_str());
            return EXIT_FAILURE;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const std::size_t build_ms = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    std::size_t lookups = 0;
    for (const std::string& p : paths) {
        Sym s;
        if (idx.lookup("fn_0", p, s).ok()) ++lookups;
    }

    std::printf("# bench_graph -- %d synthetic C files (%d fns x %d calls each)\n",
                kFiles, kFns, kCalls);
    std::printf("build_ms=%zu lookups=%zu extract=%u\n", build_ms, lookups,
                idx.extract_count());
    std::printf("syms=%zu deps=%zu files=%zu rss_kb=%zu\n", idx.sym_count(),
                idx.dep_count(), idx.file_count(), rss_kb());
    std::printf("version=%u\n", idx.version());

    const int r_done = std::system(("rm -rf " + dir).c_str());
    (void)r_done;
    return EXIT_SUCCESS;
}
