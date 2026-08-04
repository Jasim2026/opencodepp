/*
 * exec/util.cpp -- shared tool helpers (path safety, file IO, subprocess).
 */
#include "tools/exec/util.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/clock.h"
#include "util/path.h"
#include "util/string.h"

namespace opencode::tools::exec {

namespace {

core::error_code io_error(const char* what) {
    return core::make_error_code(core::Err::e_internal,
                                 static_cast<std::uint32_t>(std::strlen(what)));
}

} /* namespace */

bool resolve_in_sandbox(std::string_view base, std::string_view path,
                        std::string& out) {
    if (path.empty() || path.front() == '/') return false;
    std::vector<std::string> check;
    bool escaped = false;
    util::split(path, "/", [&](std::string_view s) {
        if (s.empty() || s == ".") return;
        if (s == "..") {
            if (check.empty()) {
                escaped = true;
                return;
            }
            check.pop_back();
            return;
        }
        check.push_back(std::string(s));
    });
    if (escaped) return false;
    std::string rel;
    for (const std::string& s : check) {
        if (!rel.empty()) rel.push_back('/');
        rel += s;
    }
    out = util::join(base, rel);
    return true;
}

core::error_code read_whole_file(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return core::make_error_code(core::Err::e_missing_cfg);
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        out.clear();
        return io_error("read");
    }
    return core::ok();
}

core::error_code write_file_atomic(const std::string& path,
                                   std::string_view content) {
    const std::string tmp = path + ".opencode_tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return io_error("create");
    const size_t n = std::fwrite(content.data(), 1, content.size(), f);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad || n != content.size()) {
        std::remove(tmp.c_str());
        return io_error("write");
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return io_error("rename");
    }
    return core::ok();
}

core::error_code file_exists(const std::string& path, bool& out) {
    struct stat st;
    out = ::stat(path.c_str(), &st) == 0;
    return core::ok();
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.emplace_back(line);
        if (nl == text.size()) break;
        pos = nl + 1;
    }
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) {
        out += l;
        out.push_back('\n');
    }
    return out;
}

bool glob_match(std::string_view pattern, std::string_view s) {
    size_t pi = 0, si = 0;
    size_t star_p = std::string_view::npos, star_s = 0;
    while (si < s.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == s[si])) {
            ++pi;
            ++si;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_p = pi++;
            star_s = si;
        } else if (star_p != std::string_view::npos) {
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

core::error_code run_process(const ProcOpts& opts, ProcResult& out) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) return io_error("pipe");
    const pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return io_error("fork");
    }
    if (pid == 0) {
        /* child */
        setpgid(0, 0);
        close(out_pipe[0]);
        close(err_pipe[0]);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(err_pipe[1], STDERR_FILENO) < 0)
            _exit(127);
        close(out_pipe[1]);
        close(err_pipe[1]);
        if (!opts.working_dir.empty() && chdir(opts.working_dir.c_str()) != 0)
            _exit(127);
        for (const std::string& kv : opts.env) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        }
        execl("/bin/sh", "sh", "-c", opts.cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    /* parent */
    close(out_pipe[1]);
    close(err_pipe[1]);
    (void)setpgid(pid, pid);

    const std::uint32_t poll_ms = 50;
    const std::uint64_t start = opencode::core::now_mono_ms();
    std::string so, se;
    int exit_code = -1;
    bool timed_out = false;
    for (;;) {
        if (opts.timeout_ms &&
            opencode::core::now_mono_ms() - start >= opts.timeout_ms) {
            (void)kill(-pid, SIGKILL);
            timed_out = true;
            break;
        }
        struct pollfd fds[2];
        nfds_t nfds = 0;
        if (out_pipe[0] >= 0) {
            fds[nfds].fd = out_pipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (err_pipe[0] >= 0) {
            fds[nfds].fd = err_pipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (nfds == 0) break;
        (void)poll(fds, nfds, static_cast<int>(poll_ms));
        char buf[4096];
        for (nfds_t i = 0; i < nfds; ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                ssize_t n;
                while ((n = read(fds[i].fd, buf, sizeof buf)) > 0) {
                    if (fds[i].fd == out_pipe[0]) so.append(buf, static_cast<size_t>(n));
                    else se.append(buf, static_cast<size_t>(n));
                }
                if (n == 0 || (fds[i].revents & POLLHUP)) {
                    close(fds[i].fd);
                    if (fds[i].fd == out_pipe[0]) out_pipe[0] = -1;
                    else err_pipe[0] = -1;
                }
            }
        }
        int st = 0;
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(st)) exit_code = WEXITSTATUS(st);
            else if (WIFSIGNALED(st)) exit_code = 128 + WTERMSIG(st);
            else exit_code = -1;
            break;
        }
    }
    /* final drain */
    char buf[4096];
    ssize_t n;
    while (out_pipe[0] >= 0 && (n = read(out_pipe[0], buf, sizeof buf)) > 0)
        so.append(buf, static_cast<size_t>(n));
    while (err_pipe[0] >= 0 && (n = read(err_pipe[0], buf, sizeof buf)) > 0)
        se.append(buf, static_cast<size_t>(n));
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    (void)waitpid(pid, nullptr, 0);

    out.exit_code = exit_code;
    out.stdout = std::move(so);
    out.stderr = std::move(se);
    out.timed_out = timed_out;
    return core::ok();
}

ArgReader::ArgReader(std::string_view args_json) {
    if (args_json.empty()) {
        root_ = util::JVal::Object({});
        return;
    }
    ec_ = parse_json(args_json, root_);
}

bool ArgReader::get_string(std::string_view key, std::string& out) const {
    const util::JVal* v = root_.find(key);
    if (!v || v->kind != util::JVal::Kind::string) return false;
    out = std::string(v->str);
    return true;
}

bool ArgReader::get_int(std::string_view key, int64_t& out) const {
    const util::JVal* v = root_.find(key);
    if (!v || v->kind != util::JVal::Kind::number) return false;
    out = static_cast<int64_t>(v->num);
    return true;
}

bool ArgReader::get_bool(std::string_view key, bool& out) const {
    const util::JVal* v = root_.find(key);
    if (!v || v->kind != util::JVal::Kind::boolean) return false;
    out = v->b;
    return true;
}

bool ArgReader::get_string_array(std::string_view key,
                                 std::vector<std::string>& out) const {
    const util::JVal* v = root_.find(key);
    if (!v || v->kind != util::JVal::Kind::array) return false;
    out.clear();
    for (const util::JVal& item : v->arr) {
        if (item.kind != util::JVal::Kind::string) return false;
        out.emplace_back(item.str);
    }
    return true;
}

std::string sym_to_json(std::string_view kind, std::string_view name,
                        std::string_view qual, std::string_view file,
                        std::int64_t line) {
    std::vector<std::pair<std::string_view, util::JVal>> obj;
    obj.emplace_back("kind", util::JVal::Str(kind));
    obj.emplace_back("name", util::JVal::Str(name));
    obj.emplace_back("qual", util::JVal::Str(qual));
    obj.emplace_back("file", util::JVal::Str(file));
    obj.emplace_back("line", util::JVal::Num(static_cast<double>(line)));
    return util::to_json(util::JVal::Object(std::move(obj)));
}

std::string snippet_to_text(std::string_view file, std::string_view sym,
                            std::string_view text) {
    std::string out = "== ";
    out.append(file);
    out.append(" :: ");
    out.append(sym);
    out.append(" ==\n");
    out.append(text);
    return out;
}

} /* namespace opencode::tools::exec */
