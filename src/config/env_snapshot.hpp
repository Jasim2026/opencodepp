/*
 * env_snapshot.hpp -- cheap, cached probes about the host environment.
 *
 * Feeds the Phase 4 network policy (edge_mode, memory) and Phase 7 context
 * assembly (git repo, shell). Every probe is non-blocking: reads tiny files
 * (/proc, .git metadata) and libc values only -- no subprocesses, no syscalls
 * that can block.
 *
 * The snapshot is built lazily on first use and cached; env_snapshot_invalidate()
 * forces a rebuild. This is the one sanctioned mutable cache in the engine
 * (the plan requires it); everything else stays instance-scoped.
 */
#ifndef OPENCODE_CONFIG_ENV_SNAPSHOT_HPP
#define OPENCODE_CONFIG_ENV_SNAPSHOT_HPP

#include <cstdint>
#include <string>

namespace opencode::config {

struct EnvSnapshot {
    std::string platform;   /* "linux" | "macos" | "windows" | "android" | "unknown" */
    std::string arch;       /* "x86_64" | "aarch64" | "arm" | "riscv" | "unknown" */
    std::string shell;      /* $SHELL or "/bin/sh" */
    bool is_git_repo = false;
    std::string git_branch; /* "(detached)" when HEAD is not on a branch */
    bool git_dirty = false; /* heuristic: index newer than HEAD (staged-proxy) */
    uint64_t free_mem_mb = 0;
    double load_avg = 0.0;  /* 1-minute load average, 0 when unavailable */
};

/* Cached snapshot. First call builds it; later calls return the same object
 * until env_snapshot_invalidate() is called. Thread-safe lazy init. */
const EnvSnapshot& env_snapshot() noexcept;

/* Force the next env_snapshot() call to re-probe. */
void env_snapshot_invalidate() noexcept;

/* Number of times the snapshot has actually been rebuilt (test hook). */
uint64_t env_snapshot_build_count() noexcept;

} /* namespace opencode::config */

#endif /* OPENCODE_CONFIG_ENV_SNAPSHOT_HPP */
