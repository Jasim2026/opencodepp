/*
 * app.hpp -- Engine assembly + lifecycle (the "app" layer).
 *
 * One Engine is one self-contained, reentrant, zero-global instance of the
 * whole stack: config -> prompt registry -> tools -> permission gate -> verify
 * gate -> store -> session -> agent loop. It is exactly what the C ABI
 * (src/abi/) wraps; hosts never construct these pieces themselves.
 *
 * The Engine never calls host code directly: hosts register callbacks (event
 * sink, permission hook, log sink) as C function pointers and the ABI layer
 * marshals them. run_task() is synchronous and runs on the caller's thread;
 * request_cancel() is the only member safe to call from another thread.
 * Never throws.
 */
#ifndef OPENCODE_APP_HPP
#define OPENCODE_APP_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/loop.h"
#include "agent/session.h"
#include "agent/states.h"
#include "config/config.hpp"
#include "core/error.h"
#include "core/event_loop.h"
#include "core/log.h"
#include "core/metrics.h"
#include "memory/entry.h"
#include "prompt/registry.h"
#include "store/store.h"
#include "tools/permission.h"
#include "tools/registry.h"
#include "verify/gate.h"

namespace opencode::app {

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /* (Re)build the engine services from a resolved Config + workspace +
     * default tool policy. Loads prompt templates (`prompt_dir` or the repo
     * layout candidates), registers the toolset over the engine's store,
     * and wires the permission/log hooks. e_invalid_cfg on bad config or
     * missing prompt templates. Never throws. */
    core::error_code configure(const config::Config& cfg, std::string_view workspace,
                               std::string_view prompt_dir, tools::Policy policy);

    /* Host hooks. Each stores a C-callback + userdata and is called only on
     * the thread that invokes run_task. Any may be NULL (no-op). */
    void set_permission_cb(tools::PermissionCallback cb, void* userdata) noexcept {
        perm_cb_ = cb;
        perm_ud_ = userdata;
        if (gate_) gate_->set_callback(cb, userdata);
    }
    void set_log_sink(core::Logger::Sink sink, void* userdata) noexcept {
        logger_.set_sink(sink, userdata);
    }
    void set_event_cb(agent::Session::EventFn fn, void* userdata) noexcept {
        event_fn_ = fn;
        event_ud_ = userdata;
    }

    const config::Config& config() const noexcept { return cfg_; }
    const std::string& workspace() const noexcept { return workspace_; }
    core::Logger* log() noexcept { return &logger_; }
    store::Store* store() const noexcept { return store_.get(); }
    core::Metrics& metrics() noexcept { return metrics_; }

    /* Run one task (user prompt) to completion on the calling thread. Returns
     * OPENCODE_ERR_BUSY semantics via DriveResult.ec == e_busy when a task is
     * already running. Never throws. */
    agent::DriveResult run_task(std::string_view prompt);

    /* Cooperative cancellation; checked at every await. Thread-safe. */
    void request_cancel() noexcept {
        if (agent_) agent_->request_cancel();
    }
    bool busy() const noexcept { return busy_.load(); }

    /* The session of the most recent run_task (nullptr until the first run). */
    agent::Session* session() const noexcept { return session_.get(); }

    /* ---- Phase 11 memory ops, workspace-scoped ---- */
    core::error_code memory_write(memory::Kind kind, std::string_view key,
                                  std::string_view value,
                                  const std::vector<std::string>& tags,
                                  std::string* out_id);
    /* Keyword/kind-scoped context text ("[kind] key: value" lines),
     * budget-bounded by the config's MemoryCfg. Empty keywords + nullopt kind
     * returns everything within budget. */
    std::string memory_context(std::optional<memory::Kind> kind,
                               const std::vector<std::string>& keywords);

private:
    void record_result(const agent::DriveResult& r);

    config::Config cfg_;
    std::string workspace_;
    core::Logger logger_;

    prompt::PromptRegistry prompts_;
    std::unique_ptr<tools::ToolRegistry> tools_;
    std::unique_ptr<tools::Gate> gate_;
    verify::Gate vgate_;
    verify::Context vctx_;
    std::unique_ptr<store::Store> store_;
    core::EventLoop loop_;

    /* Per-task session/agent (rebuilt on every run_task). */
    std::unique_ptr<agent::Session> session_;
    std::unique_ptr<agent::Agent> agent_;

    /* Host hooks (marshalled by the ABI layer). */
    tools::PermissionCallback perm_cb_ = nullptr;
    void* perm_ud_ = nullptr;
    agent::Session::EventFn event_fn_ = nullptr;
    void* event_ud_ = nullptr;

    /* Engine metrics (T2 -- counters/gauges over completed tasks). */
    core::Metrics metrics_;
    std::atomic<bool> busy_{false};
    std::uint64_t tasks_total_ = 0;
    std::uint64_t tokens_total_ = 0;
    std::uint64_t iterations_total_ = 0;
    std::uint64_t edits_total_ = 0;
};

} /* namespace opencode::app */

#endif /* OPENCODE_APP_HPP */
