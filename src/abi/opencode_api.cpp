/*
 * opencode_api.cpp -- the C ABI implementation (FROZEN v1, see opencode.h).
 *
 * Every exported symbol maps to the Engine assembly (src/app.hpp). The handle
 * struct behind the opaque `opencode_engine_t` lives here; hosts hold the
 * opaque handle. All callbacks fire on the thread that called
 * opencode_engine_run; opencode_engine_cancel is the single thread-safe
 * entry point. Nothing here throws across the boundary (try/catch -> status).
 */
#include "opencode/opencode.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "abi/opencode_events.h"
#include "agent/session.h"
#include "agent/states.h"
#include "app.hpp"
#include "config/config.hpp"
#include "core/error.h"
#include "core/log.h"
#include "memory/entry.h"
#include "tools/permission.h"
#include "util/json.h"

namespace {

using opencode::agent::AgentEvent;
using opencode::agent::AgentState;
using opencode::app::Engine;
using opencode::config::Config;
using opencode::core::Err;
using opencode::core::Level;
using opencode::core::error_code;
using opencode::memory::Kind;
using opencode::tools::Policy;

/* The handle: engine instance + the host's registered C callbacks. */
struct EngineHandle {
    Engine engine;
    opencode_event_fn event_fn = nullptr;
    opencode_permission_fn perm_fn = nullptr;
    opencode_consent_fn consent_fn = nullptr;
    opencode_log_fn log_fn = nullptr;
    void* userdata = nullptr;
    uint32_t task_seq = 0; /* engine-assigned session id for events */
};

EngineHandle* as_engine(opencode_engine_t* e) noexcept {
    return reinterpret_cast<EngineHandle*>(e);
}

opencode_status_t status_of(const error_code& ec) noexcept {
    return ec.ok() ? OPENCODE_OK : opencode::core::to_abi_status(ec.code());
}

/* ---- host-callback adapters (engine types -> C types) ---- */

/* agent::Session::EventFn: marshal to opencode_event_t; a non-OK return from
 * the host cancels the running task at the next safe point. */
void dispatch_event(void* userdata, const AgentEvent& ev) noexcept {
    auto* h = static_cast<EngineHandle*>(userdata);
    if (h->event_fn == nullptr) return;
    char text[OPENCODE_EVENT_TEXT_MAX];
    opencode_event_t cev{};
    cev.text = text;
    cev.text_cap = sizeof text;
    opencode::abi::marshal(ev, &cev, h->task_seq);
    if (h->event_fn(h->userdata, &cev) != OPENCODE_OK) h->engine.request_cancel();
}

/* tools::PermissionCallback: return true to allow the tool call. A host that
 * registered no permission hook denies everything (fail closed). */
bool dispatch_permission(void* userdata, std::string_view tool,
                         std::string_view args) noexcept {
    auto* h = static_cast<EngineHandle*>(userdata);
    if (h->perm_fn == nullptr) return false;
    const std::string t(tool);
    const std::string a(args);
    return h->perm_fn(h->userdata, t.c_str(), a.c_str()) != 0;
}

/* core::Logger::Sink: route to the host's opencode_log_fn. */
void dispatch_log(void* userdata, Level level, std::string_view line) noexcept {
    auto* h = static_cast<EngineHandle*>(userdata);
    if (h->log_fn == nullptr) return;
    int lvl = 1; /* 0=debug 1=info 2=warn 3=error */
    switch (level) {
        case Level::trace:
        case Level::debug: lvl = 0; break;
        case Level::info:  lvl = 1; break;
        case Level::warn:  lvl = 2; break;
        case Level::error: lvl = 3; break;
    }
    const std::string msg(line);
    h->log_fn(h->userdata, lvl, msg.c_str());
}

/* ---- config conversion ---- */

Policy policy_from(opencode_tool_policy_t p) noexcept {
    switch (p) {
        case OPENCODE_POLICY_DENY: return Policy::deny;
        case OPENCODE_POLICY_ASK: return Policy::ask;
        case OPENCODE_POLICY_ALLOW: return Policy::allow;
        case OPENCODE_POLICY_ALLOW_READONLY: return Policy::allow_readonly;
    }
    return Policy::deny;
}

/* Overlay a single provider entry onto `cfg`: replace the provider with the
 * given id (if present) or append it. Only the fields the host supplied are
 * written. */
void overlay_provider(Config& cfg, const opencode_config_t& c) {
    const std::string id = c.provider != nullptr ? c.provider : "openai_compat";
    const std::string base =
        c.base_url != nullptr ? c.base_url : "http://127.0.0.1:8080";
    const std::string key = c.api_key != nullptr ? c.api_key : "sk-test";
    const std::string model = c.model != nullptr ? c.model : "mock-model";

    for (opencode::config::ProviderCfg& p : cfg.providers) {
        if (p.id == id) {
            if (c.base_url != nullptr) p.base_url = base;
            if (c.api_key != nullptr) p.api_key = key;
            if (c.model != nullptr) p.default_model = model;
            return;
        }
    }
    opencode::config::ProviderCfg p;
    p.id = id;
    p.base_url = base;
    p.api_key = key;
    p.default_model = model;
    cfg.providers.push_back(std::move(p));
}

void overlay_agent(Config& cfg, const opencode_config_t& c) {
    const std::string id = c.agent != nullptr ? c.agent : "default";
    for (opencode::config::AgentCfg& a : cfg.agents) {
        if (a.id == id) {
            if (c.model != nullptr) a.model = c.model;
            return;
        }
    }
    opencode::config::AgentCfg a;
    a.id = id;
    a.model = c.model != nullptr ? c.model : "mock-model";
    cfg.agents.push_back(std::move(a));
}

opencode_status_t apply_config(EngineHandle* h,
                               const opencode_config_t& c) noexcept {
    if (c.version != OPENCODE_CONFIG_VERSION)
        return OPENCODE_ERR_VALIDATION;

    Config cfg;
    error_code ec;
    if (c.config_path != nullptr) {
        ec = opencode::config::load_config_file(c.config_path, cfg,
                                                h->engine.log());
        if (!ec.ok()) return status_of(ec);
    }
    overlay_provider(cfg, c);
    overlay_agent(cfg, c);
    if (c.network_timeout_ms != 0) cfg.network.timeout_ms = c.network_timeout_ms;
    if (c.memory_max_entries != 0) cfg.memory.max_entries = c.memory_max_entries;
    if (c.memory_max_entries_per_task != 0)
        cfg.memory.max_entries_per_task = c.memory_max_entries_per_task;
    if (c.memory_max_value_chars != 0)
        cfg.memory.max_value_chars = c.memory_max_value_chars;

    const std::string workspace = c.workspace != nullptr ? c.workspace : ".";
    const std::string prompt_dir =
        c.prompt_dir != nullptr ? c.prompt_dir : "";

    h->event_fn = c.on_event;
    h->perm_fn = c.on_permission;
    h->consent_fn = c.on_consent;
    h->log_fn = c.on_log;
    h->userdata = c.userdata;

    h->engine.set_permission_cb(&dispatch_permission, h);
    h->engine.set_log_sink(&dispatch_log, h);

    ec = h->engine.configure(cfg, workspace, prompt_dir,
                             policy_from(c.tool_policy));
    return status_of(ec);
}

/* ---- JSON helpers ---- */

/* Parse a JSON array of strings into `out`; ok() when `json` is NULL/empty. */
error_code parse_tags(std::string_view json, std::vector<std::string>& out) {
    if (json.empty()) return opencode::core::ok();
    opencode::util::JVal root;
    std::size_t pos = 0;
    const error_code ec =
        opencode::util::parse_json(json, root, &pos);
    if (!ec.ok()) return ec;
    if (root.kind != opencode::util::JVal::Kind::array)
        return opencode::core::make_error_code(Err::e_invalid_cfg);
    for (const opencode::util::JVal& v : root.arr) {
        if (v.kind != opencode::util::JVal::Kind::string)
            return opencode::core::make_error_code(Err::e_invalid_cfg);
        out.emplace_back(v.str);
    }
    return opencode::core::ok();
}

std::optional<Kind> kind_from_abi(opencode_memory_kind_t k) noexcept {
    if (k >= OPENCODE_MEMORY_DECISION && k <= OPENCODE_MEMORY_USER_PREF)
        return static_cast<Kind>(k);
    return std::nullopt;
}

/* Copy `src` into a caller-owned buffer, NUL-terminated, bounded by `cap`.
 * Returns bytes copied (excluding NUL) via *out_len when non-NULL. */
void copy_out(std::string_view src, char* out, size_t cap, size_t* out_len) {
    if (out == nullptr || cap == 0) {
        if (out_len != nullptr) *out_len = src.size();
        return;
    }
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memcpy(out, src.data(), n);
    out[n] = '\0';
    if (out_len != nullptr) *out_len = n;
}

} /* namespace */

extern "C" {

opencode_status_t opencode_engine_create(const opencode_config_t* cfg,
                                         opencode_engine_t** out) {
    if (out == nullptr) return OPENCODE_ERR_VALIDATION;
    *out = nullptr;
    try {
        auto* h = new (std::nothrow) EngineHandle{};
        if (h == nullptr) return OPENCODE_ERR_FATAL;
        if (cfg != nullptr) {
            const opencode_status_t st = apply_config(h, *cfg);
            if (st != OPENCODE_OK) {
                delete h;
                return st;
            }
        }
        *out = reinterpret_cast<opencode_engine_t*>(h);
        return OPENCODE_OK;
    } catch (...) {
        return OPENCODE_ERR_FATAL;
    }
}

opencode_status_t opencode_engine_destroy(opencode_engine_t* eng) {
    if (eng == nullptr) return OPENCODE_ERR_VALIDATION;
    delete as_engine(eng);
    return OPENCODE_OK;
}

opencode_status_t opencode_engine_set_config(opencode_engine_t* eng,
                                             const opencode_config_t* cfg) {
    if (eng == nullptr || cfg == nullptr) return OPENCODE_ERR_VALIDATION;
    auto* h = as_engine(eng);
    if (h->engine.busy()) return OPENCODE_ERR_BUSY;
    try {
        return apply_config(h, *cfg);
    } catch (...) {
        return OPENCODE_ERR_FATAL;
    }
}

opencode_status_t opencode_engine_drive(opencode_engine_t* eng,
                                        int32_t /*wait_ms*/) {
    if (eng == nullptr) return OPENCODE_ERR_VALIDATION;
    /* v1 tasks run synchronously on the caller's thread; there is nothing to
     * pump. BUSY while a task owns the engine, OK when idle. */
    return as_engine(eng)->engine.busy() ? OPENCODE_ERR_BUSY : OPENCODE_OK;
}

opencode_status_t opencode_engine_run(opencode_engine_t* eng,
                                      const char* prompt,
                                      opencode_event_fn on_event,
                                      void* userdata) {
    if (eng == nullptr || prompt == nullptr) return OPENCODE_ERR_VALIDATION;
    auto* h = as_engine(eng);
    if (h->engine.busy()) return OPENCODE_ERR_BUSY;

    try {
        const opencode_event_fn prev = h->event_fn;
        void* prev_ud = h->userdata;
        if (on_event != nullptr) {
            h->event_fn = on_event;
            h->userdata = userdata;
        }
        h->engine.set_event_cb(&dispatch_event, h);
        ++h->task_seq;

        const error_code ec = h->engine.run_task(prompt).ec;

        if (on_event != nullptr) {
            h->event_fn = prev;
            h->userdata = prev_ud;
        }
        return status_of(ec);
    } catch (...) {
        return OPENCODE_ERR_FATAL;
    }
}

opencode_status_t opencode_engine_cancel(opencode_engine_t* eng) {
    if (eng == nullptr) return OPENCODE_ERR_VALIDATION;
    as_engine(eng)->engine.request_cancel();
    return OPENCODE_OK;
}

opencode_status_t opencode_metrics_snapshot(opencode_engine_t* eng,
                                            opencode_metric_fn sink,
                                            void* userdata,
                                            uint32_t* out_count) {
    if (eng == nullptr || sink == nullptr) return OPENCODE_ERR_VALIDATION;
    auto* h = as_engine(eng);
    uint32_t n = 0;
    h->engine.metrics().snapshot([&](std::string_view name,
                                     opencode::core::Metrics::Kind kind,
                                     double value, uint64_t count) {
        opencode_metric_kind_t ck = OPENCODE_METRIC_COUNTER;
        switch (kind) {
            case opencode::core::Metrics::Kind::counter: ck = OPENCODE_METRIC_COUNTER; break;
            case opencode::core::Metrics::Kind::gauge: ck = OPENCODE_METRIC_GAUGE; break;
            case opencode::core::Metrics::Kind::histogram: ck = OPENCODE_METRIC_HISTOGRAM; break;
        }
        ++n;
        const std::string name_nul(name);
        sink(userdata, name_nul.c_str(), ck, value, count);
    });
    if (out_count != nullptr) *out_count = n;
    return OPENCODE_OK;
}

opencode_status_t opencode_memory_write(opencode_engine_t* eng,
                                        opencode_memory_kind_t kind,
                                        const char* key, const char* value,
                                        const char* tags_json,
                                        char* out_id, size_t out_id_cap) {
    if (eng == nullptr || key == nullptr || value == nullptr)
        return OPENCODE_ERR_VALIDATION;
    const std::optional<Kind> k = kind_from_abi(kind);
    if (!k.has_value()) return OPENCODE_ERR_VALIDATION;

    std::vector<std::string> tags;
    const error_code ec = parse_tags(
        tags_json != nullptr ? std::string_view(tags_json) : std::string_view{},
        tags);
    if (!ec.ok()) return status_of(ec);

    std::string id;
    const error_code wec =
        as_engine(eng)->engine.memory_write(*k, key, value, tags, &id);
    if (!wec.ok()) return status_of(wec);
    copy_out(id, out_id, out_id_cap, nullptr);
    return OPENCODE_OK;
}

opencode_status_t opencode_memory_read(opencode_engine_t* eng,
                                       opencode_memory_kind_t kind,
                                       const char* keywords_json,
                                       char* out, size_t out_cap,
                                       size_t* out_len) {
    if (eng == nullptr) return OPENCODE_ERR_VALIDATION;
    const std::optional<Kind> k = kind_from_abi(kind);
    if (!k.has_value()) return OPENCODE_ERR_VALIDATION;

    std::vector<std::string> keywords;
    const error_code ec = parse_tags(
        keywords_json != nullptr ? std::string_view(keywords_json)
                                 : std::string_view{},
        keywords);
    if (!ec.ok()) return status_of(ec);

    const std::string ctx =
        as_engine(eng)->engine.memory_context(k, keywords);
    copy_out(ctx, out, out_cap, out_len);
    return OPENCODE_OK;
}

} /* extern "C" */
