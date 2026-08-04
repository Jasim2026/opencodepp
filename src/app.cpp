/* app.cpp -- Engine assembly (see app.hpp). Never throws. */
#include "app.hpp"

#include <cstdio>

#include "memory/workspace_memory.h"
#include "store/mem_store.hpp"

namespace opencode::app {

namespace {

/* Candidate prompt-template directories when no prompt_dir is configured
 * (repo-root layout; matches tools/run_agent). */
constexpr const char* kTemplateDirs[] = {
    "src/prompt/templates", "../src/prompt/templates",
    "opencodepp/src/prompt/templates"};

constexpr core::error_code err(core::Err e) noexcept {
    return core::make_error_code(e);
}

} /* namespace */

Engine::Engine() { store_ = opencode::store::create_mem_store(); }

Engine::~Engine() = default;

core::error_code Engine::configure(const config::Config& cfg,
                                   std::string_view workspace,
                                   std::string_view prompt_dir,
                                   tools::Policy policy) {
    cfg_ = cfg;
    workspace_ = std::string(workspace);

    /* Prompt templates: configured dir first, then the repo layout. */
    if (!prompt_dir.empty()) {
        if (!prompt::load_templates(std::string(prompt_dir), prompts_).ok())
            return err(core::Err::e_invalid_cfg);
    } else {
        bool loaded = false;
        for (const char* dir : kTemplateDirs) {
            if (prompt::load_templates(dir, prompts_).ok()) {
                loaded = true;
                break;
            }
        }
        if (!loaded) return err(core::Err::e_invalid_cfg);
    }

    /* Tools are registered once over the engine-owned store (stable across
     * sessions). include_write=true; graph wiring is Phase 7 (ContextSlice). */
    tools_ = std::make_unique<tools::ToolRegistry>();
    const core::error_code ec = tools::register_defaults(
        *tools_, {workspace_, nullptr, true, store_.get(), cfg_.memory});
    if (!ec.ok()) return ec;

    gate_ = std::make_unique<tools::Gate>(policy);
    gate_->set_callback(perm_cb_, perm_ud_);

    vctx_ = verify::Context{};
    vctx_.workspace_root = workspace_;

    return core::ok();
}

agent::DriveResult Engine::run_task(std::string_view prompt) {
    agent::DriveResult fail;
    fail.ec = err(core::Err::e_busy);
    if (busy_.exchange(true)) return fail;

    /* Fresh session per task (the ABI owns task-level state). */
    session_.reset();
    agent_.reset();

    agent::SessionOptions sopts;
    if (const core::error_code ec =
            agent::session_options_from_config(cfg_, /*agent_id=*/"",
                                               workspace_, sopts);
        !ec.ok()) {
        busy_.store(false);
        fail.ec = ec;
        return fail;
    }
    sopts.log = &logger_;
    sopts.store = store_.get();

    session_ = std::make_unique<agent::Session>(std::move(sopts));
    if (event_fn_ != nullptr) session_->set_event_fn(event_fn_, event_ud_);

    agent::LoopOptions lo;
    lo.loop = &loop_;
    lo.prompt = &prompts_;
    lo.tools = tools_.get();
    lo.permission = gate_.get();
    lo.verify = &vgate_;
    lo.verify_ctx = vctx_;
    lo.retry.max_retries = cfg_.network.max_retries;
    lo.connect_timeout_ms = cfg_.network.timeout_ms;
    lo.request_timeout_ms = cfg_.network.timeout_ms;

    agent_ = std::make_unique<agent::Agent>(*session_, lo);
    const agent::DriveResult r = agent_->drive(prompt);

    busy_.store(false);
    record_result(r);
    return r;
}

void Engine::record_result(const agent::DriveResult& r) {
    ++tasks_total_;
    tokens_total_ += r.tokens_used;
    iterations_total_ += r.iterations;
    edits_total_ += r.applied_edits.size();

    metrics_.inc("engine.tasks");
    metrics_.inc("engine.tokens_total", r.tokens_used);
    metrics_.set("engine.tokens_used", static_cast<std::int64_t>(r.tokens_used));
    metrics_.set("engine.iterations", static_cast<std::int64_t>(r.iterations));
    metrics_.inc("engine.edits", r.applied_edits.size());
}

core::error_code Engine::memory_write(memory::Kind kind, std::string_view key,
                                      std::string_view value,
                                      const std::vector<std::string>& tags,
                                      std::string* out_id) {
    memory::Entry e;
    e.kind = kind;
    e.key = std::string(key);
    e.value = std::string(value);
    e.tags = tags;
    e.source = "abi";
    return memory::write_entry(store_.get(), workspace_, std::move(e),
                               cfg_.memory, out_id);
}

std::string Engine::memory_context(std::optional<memory::Kind> kind,
                                   const std::vector<std::string>& keywords) {
    std::vector<memory::Entry> entries;
    if (keywords.empty()) {
        entries = memory::read_entries(store_.get(), workspace_, cfg_.memory,
                                       kind);
    } else {
        entries = memory::match_entries(store_.get(), workspace_, keywords,
                                        cfg_.memory);
        if (kind.has_value()) {
            std::vector<memory::Entry> filtered;
            filtered.reserve(entries.size());
            for (const memory::Entry& e : entries) {
                if (e.kind == *kind) filtered.push_back(e);
            }
            entries = std::move(filtered);
        }
    }
    return memory::entries_to_context(entries, cfg_.memory.max_entries_per_task,
                                      cfg_.memory.max_entry_tokens,
                                      cfg_.memory.max_value_chars);
}

} /* namespace opencode::app */
