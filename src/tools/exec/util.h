/*
 * exec/util.h -- shared helpers for the tool implementations.
 *
 * Path safety lives here: every file path a tool touches is resolved against
 * the configured workspace base and refused when it would escape it (lexical
 * normalization, no symlink following). The subprocess runner is the ONLY
 * process-spawning helper in the tools module (the permission gate is the only
 * entry point that reaches it). Never throws.
 */
#ifndef OPENCODE_TOOLS_EXEC_UTIL_H
#define OPENCODE_TOOLS_EXEC_UTIL_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "tools/tool.h"
#include "util/json.h"

namespace opencode::tools::exec {

/* Lexically normalize `path` (resolve '.', '..', '//') against the sandbox
 * `base` and return the joined absolute path. Returns false when `path` is
 * absolute, empty, or escapes above `base`. */
bool resolve_in_sandbox(std::string_view base, std::string_view path,
                        std::string& out);

core::error_code read_whole_file(const std::string& path, std::string& out);
core::error_code write_file_atomic(const std::string& path,
                                   std::string_view content);
core::error_code file_exists(const std::string& path, bool& out);

/* Text line helpers. split_lines drops the '\n' (and a trailing '\r'); the
 * trailing empty element represents the EOF newline, so join_lines round-trips
 * any text exactly. */
std::vector<std::string> split_lines(std::string_view text);
std::string join_lines(const std::vector<std::string>& lines);

/* fnmatch-lite: `*` and `?`, case-sensitive. */
bool glob_match(std::string_view pattern, std::string_view s);

struct ProcOpts {
    std::string cmd;
    std::string working_dir;
    std::vector<std::string> env; /* "KEY=VAL" overrides on top of the host env */
    std::uint32_t timeout_ms = 0; /* 0 = no timeout */
};
struct ProcResult {
    int exit_code = -1;
    std::string stdout;
    std::string stderr;
    bool timed_out = false;
};
/* POSIX subprocess runner: /bin/sh -c cmd. Non-streaming; timeouts kill the
 * child process group. Streaming + cancellation land in the Phase 8
 * cancellation/streaming commit. */
core::error_code run_process(const ProcOpts& opts, ProcResult& out);

/* Tiny argument reader over an invocation's args_json. Optional ints/bools
 * keep their defaults when the key is absent. */
class ArgReader {
public:
    explicit ArgReader(std::string_view args_json);

    bool ok() const noexcept { return ec_.ok(); }
    core::error_code error() const noexcept { return ec_; }

    bool get_string(std::string_view key, std::string& out) const;
    bool get_int(std::string_view key, int64_t& out) const;
    bool get_bool(std::string_view key, bool& out) const;
    bool get_string_array(std::string_view key,
                          std::vector<std::string>& out) const;

private:
    util::JVal root_;
    core::error_code ec_;
};

/* Short renderers for structured tool payloads. */
std::string sym_to_json(std::string_view kind, std::string_view name,
                        std::string_view qual, std::string_view file,
                        std::int64_t line);
std::string snippet_to_text(std::string_view file, std::string_view sym,
                            std::string_view text);

} /* namespace opencode::tools::exec */

#endif /* OPENCODE_TOOLS_EXEC_UTIL_H */
