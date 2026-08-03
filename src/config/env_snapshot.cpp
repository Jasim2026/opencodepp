/*
 * env_snapshot.cpp -- see env_snapshot.hpp. All probes are non-blocking.
 */
#include "config/env_snapshot.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace opencode::config {
namespace {

const char* detect_platform() noexcept {
#if defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

const char* detect_arch() noexcept {
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__riscv)
    return "riscv";
#else
    return "unknown";
#endif
}

/* Read a small file into `out`. Returns false when unreadable. Never blocks
 * (the targets are regular files or /proc pseudo-files). */
bool read_small_file(const char* path, std::string& out) noexcept {
#if defined(__unix__) || defined(__APPLE__)
    char buf[4096];
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out.clear();
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
        } else {
            const bool ok = (n == 0);
            ::close(fd);
            return ok;
        }
    }
#else
    (void)path;
    (void)out;
    return false;
#endif
}

#if defined(__unix__) || defined(__APPLE__)
bool file_mtime(const char* path, long long& mtime) noexcept {
    struct stat st;
    if (::stat(path, &st) != 0) return false;
    mtime = static_cast<long long>(st.st_mtime);
    return true;
}
#endif

/* Walk up from the CWD looking for a ".git" entry. On success fills
 * `git_dir` (the full path to the .git entry) and the staged-change proxy for
 * `dirty`. Returns false when no repository is found. */
bool probe_git(std::string& git_dir, bool& dirty) noexcept {
#if defined(__unix__) || defined(__APPLE__)
    char cwd[4096];
    if (::getcwd(cwd, sizeof cwd) == nullptr) return false;
    std::string dir = cwd;
    for (;;) {
        const std::string candidate = dir + "/.git";
        struct stat st;
        if (::stat(candidate.c_str(), &st) == 0) {
            /* index-newer-than-HEAD is a staged-change proxy for "dirty". It
             * intentionally does not track unstaged edits (that would require
             * spawning `git status`, which is blocking). */
            long long head_mt = 0, index_mt = 0;
            const std::string head = candidate + "/HEAD";
            const std::string index = candidate + "/index";
            if (file_mtime(head.c_str(), head_mt) &&
                file_mtime(index.c_str(), index_mt)) {
                dirty = index_mt > head_mt;
            } else {
                dirty = false;
            }
            git_dir = candidate;
            return true;
        }
        const size_t slash = dir.rfind('/');
        if (slash == std::string::npos) return false;
        dir = dir.substr(0, slash);
        if (dir.empty()) return false;
    }
#else
    (void)git_dir;
    (void)dirty;
    return false;
#endif
}

/* Branch name from .git/HEAD: "ref: refs/heads/<branch>" or the commit id
 * when detached. Returns false when HEAD is unreadable. */
bool read_git_branch(const std::string& git_dir, std::string& branch) noexcept {
#if defined(__unix__) || defined(__APPLE__)
    std::string head;
    if (!read_small_file((git_dir + "/HEAD").c_str(), head)) return false;
    const std::string_view sv(head.data(), head.size());
    constexpr std::string_view ref = "ref: refs/heads/";
    const size_t pos = sv.find(ref);
    if (pos != std::string_view::npos) {
        branch.assign(sv.substr(pos + ref.size()));
        while (!branch.empty() &&
               (branch.back() == '\n' || branch.back() == '\r' ||
                branch.back() == ' ')) {
            branch.pop_back();
        }
        return true;
    }
    const size_t end = sv.find_first_of("\n\r ");
    const size_t len = (end == std::string_view::npos) ? sv.size() : end;
    if (len == 0) return false;
    branch = std::string(sv.data(), len);
    if (branch.size() > 12) branch.resize(12);
    branch.insert(0, "(detached) ");
    return true;
#else
    (void)git_dir;
    (void)branch;
    return false;
#endif
}

void probe_memory_load(EnvSnapshot& out) noexcept {
#if defined(__linux__)
    std::string meminfo;
    if (read_small_file("/proc/meminfo", meminfo)) {
        const std::string_view sv(meminfo.data(), meminfo.size());
        const size_t p = sv.find("MemAvailable:");
        if (p != std::string_view::npos) {
            size_t q = p + 13;
            while (q < sv.size() && sv[q] == ' ') ++q;
            uint64_t kb = 0;
            while (q < sv.size() && sv[q] >= '0' && sv[q] <= '9') {
                kb = kb * 10 + static_cast<uint64_t>(sv[q] - '0');
                ++q;
            }
            out.free_mem_mb = kb / 1024;
        }
    }
    std::string load;
    if (read_small_file("/proc/loadavg", load)) {
        const std::string_view sv(load.data(), load.size());
        const std::string first(sv.substr(0, sv.find(' ')));
        out.load_avg = std::strtod(first.c_str(), nullptr);
    }
#else
    (void)out;
#endif
}

void build(EnvSnapshot& out) noexcept {
    out.platform = detect_platform();
    out.arch = detect_arch();
    const char* sh = std::getenv("SHELL");
    out.shell = (sh != nullptr && *sh != '\0') ? sh : "/bin/sh";
    std::string git_dir;
    bool dirty = false;
    if (probe_git(git_dir, dirty)) {
        out.is_git_repo = true;
        out.git_dirty = dirty;
        if (!read_git_branch(git_dir, out.git_branch)) out.git_branch.clear();
    }
    probe_memory_load(out);
}

struct SnapshotCache {
    EnvSnapshot snap;
    bool built = false;
    uint64_t builds = 0;
};

SnapshotCache& cache() noexcept {
    static SnapshotCache c;
    return c;
}

std::mutex& cache_mu() noexcept {
    static std::mutex m;
    return m;
}

} /* namespace */

const EnvSnapshot& env_snapshot() noexcept {
    std::lock_guard<std::mutex> lock(cache_mu());
    SnapshotCache& c = cache();
    if (!c.built) {
        build(c.snap);
        c.built = true;
        ++c.builds;
    }
    return c.snap;
}

void env_snapshot_invalidate() noexcept {
    std::lock_guard<std::mutex> lock(cache_mu());
    cache().built = false;
}

uint64_t env_snapshot_build_count() noexcept {
    std::lock_guard<std::mutex> lock(cache_mu());
    return cache().builds;
}

} /* namespace opencode::config */
