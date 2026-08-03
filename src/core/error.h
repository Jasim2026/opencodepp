/*
 * error.h -- the single error vocabulary for the whole project.
 *
 * Every error in OpenCode++ is an `Err` value classified by `retry_class`:
 *   - none       : deterministic; do NOT retry (auth, validation, config...)
 *   - retryable  : transient; may retry under the budget policy (net, rate...)
 *   - fatal      : invariant broken; abort the task, report, never swallow
 *
 * error_code is a small, copyable, allocation-free error object modeled on
 * std::error_code, plus a mapping to the ABI status enum (opencode.h).
 *
 * Rules (per 02_CODING_PROTOCOL.md Section 1.4): no error path is ever swallowed; a
 * function either returns an error_code, logs, or calls the host error
 * callback. `if (err) {}` is forbidden.
 */
#ifndef OPENCODE_CORE_ERROR_H
#define OPENCODE_CORE_ERROR_H

#include <cstdint>
#include <string_view>
#include <utility>

#include "opencode/opencode.h" /* opencode_status_t */

namespace opencode::core {

/* Stable error categories (ids are part of the serialized surface). */
enum class ErrCat : uint8_t {
    net      = 1,
    provider = 2,
    config   = 3,
    core     = 4,
    tool     = 5,
    verify   = 6,
    agent    = 7,
    abi      = 8,
};

/* The complete error vocabulary. Additive-only; never renumber. */
enum class Err : int {
    ok = 0,

    /* net */
    e_net_connect   = 1, /* TCP connect failed/timed out              */
    e_net_resolve   = 2, /* DNS resolution failed                     */
    e_net_tls       = 3, /* TLS handshake/record failure              */
    e_net_http      = 4, /* non-2xx/3xx HTTP status                   */
    e_net_timeout   = 5, /* read/write/pool wait timed out            */
    e_net_overflow  = 6, /* response exceeded configured cap          */
    e_net_offline   = 7, /* no connectivity; queued for resume        */

    /* provider */
    e_proto_parse   = 8,  /* malformed wire payload                   */
    e_auth          = 9,  /* bad/missing credentials                  */
    e_rate_limit    = 10, /* 429 / quota                             */
    e_provider_err  = 11, /* provider-level error (4xx/5xx body)      */
    e_model_unsup   = 12, /* model id not in catalog                  */

    /* config */
    e_invalid_cfg   = 13, /* malformed/contradictory config           */
    e_missing_cfg   = 14, /* required field absent                    */

    /* core */
    e_busy          = 15, /* engine already has an active session     */
    e_cancelled     = 16, /* cancelled by host or budget              */
    e_internal      = 17, /* invariant broken (fatal)                 */
    e_not_impl      = 18, /* requested feature not yet implemented    */
    e_oom           = 19, /* allocation failed                        */

    /* tool / verify / agent */
    e_tool_reject   = 20, /* tool call denied by policy/host          */
    e_tool_notfound = 21, /* unknown tool id                          */
    e_verify_fail   = 22, /* an edit failed a verification gate       */
    e_budget        = 23, /* per-task token budget exhausted          */
    e_aborted       = 24, /* loop aborted by feedback dedupe          */
    e_overflow      = 25, /* caller-provided buffer too small         */

    last_,
};

enum class Retry : uint8_t { none = 0, retryable = 1, fatal = 2 };

constexpr ErrCat category(Err e) noexcept {
    switch (e) {
        case Err::e_net_connect: case Err::e_net_resolve: case Err::e_net_tls:
        case Err::e_net_http:    case Err::e_net_timeout: case Err::e_net_overflow:
        case Err::e_net_offline: return ErrCat::net;
        case Err::e_proto_parse: case Err::e_auth: case Err::e_rate_limit:
        case Err::e_provider_err: case Err::e_model_unsup: return ErrCat::provider;
        case Err::e_invalid_cfg: case Err::e_missing_cfg:  return ErrCat::config;
        case Err::e_busy: case Err::e_cancelled: case Err::e_internal:
        case Err::e_not_impl: case Err::e_oom: case Err::e_overflow:
            return ErrCat::core;
        case Err::e_tool_reject: case Err::e_tool_notfound: return ErrCat::tool;
        case Err::e_verify_fail:                           return ErrCat::verify;
        case Err::e_budget: case Err::e_aborted:           return ErrCat::agent;
        case Err::ok: case Err::last_: break;
    }
    return ErrCat::core;
}

/* Retry classification -- the one place that decides "can we try again?". */
constexpr Retry retry_class(Err e) noexcept {
    switch (e) {
        /* transient: safe to retry under the budget policy */
        case Err::e_net_connect: case Err::e_net_resolve: case Err::e_net_tls:
        case Err::e_net_http:    case Err::e_net_timeout: case Err::e_net_overflow:
        case Err::e_net_offline: case Err::e_rate_limit: case Err::e_provider_err:
            return Retry::retryable;

        /* deterministic: retrying cannot help */
        case Err::e_auth: case Err::e_invalid_cfg: case Err::e_missing_cfg:
        case Err::e_model_unsup: case Err::e_tool_reject: case Err::e_tool_notfound:
        case Err::e_proto_parse: case Err::e_verify_fail: case Err::e_budget:
        case Err::e_aborted: case Err::e_busy: case Err::e_not_impl:
        case Err::e_overflow:
            return Retry::none;

        /* invariant broken: never retry, surface to host */
        case Err::e_internal: case Err::e_oom:
            return Retry::fatal;

        case Err::ok: case Err::last_: case Err::e_cancelled:
            return Retry::none;
    }
    return Retry::fatal;
}

/* Human-readable name for diagnostics / logs. */
constexpr std::string_view err_name(Err e) noexcept {
    switch (e) {
        case Err::ok: return "ok";
        case Err::e_net_connect: return "net_connect";
        case Err::e_net_resolve: return "net_resolve";
        case Err::e_net_tls: return "net_tls";
        case Err::e_net_http: return "net_http";
        case Err::e_net_timeout: return "net_timeout";
        case Err::e_net_overflow: return "net_overflow";
        case Err::e_net_offline: return "net_offline";
        case Err::e_proto_parse: return "proto_parse";
        case Err::e_auth: return "auth";
        case Err::e_rate_limit: return "rate_limit";
        case Err::e_provider_err: return "provider_err";
        case Err::e_model_unsup: return "model_unsupported";
        case Err::e_invalid_cfg: return "invalid_config";
        case Err::e_missing_cfg: return "missing_config";
        case Err::e_busy: return "busy";
        case Err::e_cancelled: return "cancelled";
        case Err::e_internal: return "internal";
        case Err::e_not_impl: return "not_implemented";
        case Err::e_oom: return "out_of_memory";
        case Err::e_tool_reject: return "tool_rejected";
        case Err::e_tool_notfound: return "tool_not_found";
        case Err::e_verify_fail: return "verify_failed";
        case Err::e_budget: return "budget_exhausted";
        case Err::e_aborted: return "aborted";
        case Err::e_overflow: return "buffer_overflow";
        case Err::last_: break;
    }
    return "unknown";
}

/* Mapping to the C ABI status enum (single source of truth). */
constexpr opencode_status_t to_abi_status(Err e) noexcept {
    switch (e) {
        case Err::ok: return OPENCODE_OK;
        case Err::e_net_connect: case Err::e_net_resolve: case Err::e_net_tls:
        case Err::e_net_http: case Err::e_net_timeout: case Err::e_net_overflow:
            return OPENCODE_ERR_NETWORK;
        case Err::e_net_offline: return OPENCODE_ERR_NO_NETWORK;
        case Err::e_auth: case Err::e_model_unsup: return OPENCODE_ERR_AUTH;
        case Err::e_invalid_cfg: case Err::e_missing_cfg: case Err::e_tool_reject:
        case Err::e_tool_notfound: case Err::e_proto_parse: case Err::e_not_impl:
            return OPENCODE_ERR_VALIDATION;
        case Err::e_busy: return OPENCODE_ERR_BUSY;
        case Err::e_cancelled: case Err::e_budget: case Err::e_aborted:
            return OPENCODE_ERR_CANCELLED;
        case Err::e_internal: case Err::e_oom: return OPENCODE_ERR_FATAL;
        case Err::e_overflow: return OPENCODE_ERR_VALIDATION;
        case Err::e_verify_fail: case Err::e_rate_limit:
        case Err::e_provider_err: case Err::last_:
            return OPENCODE_ERR_VALIDATION;
    }
    return OPENCODE_ERR_FATAL;
}

/* Small, copyable, allocation-free error object (std::error_code-style). */
class error_code {
public:
    constexpr error_code() noexcept : code_(Err::ok), detail_(0) {}
    constexpr explicit error_code(Err code, uint32_t detail = 0) noexcept
        : code_(code), detail_(detail) {}

    bool ok() const noexcept { return code_ == Err::ok; }
    explicit operator bool() const noexcept { return !ok(); }

    Err       code() const noexcept { return code_; }
    ErrCat    cat() const noexcept { return category(code_); }
    Retry     retry() const noexcept { return retry_class(code_); }
    uint32_t  detail() const noexcept { return detail_; }

    std::string_view message() const noexcept { return err_name(code_); }
    opencode_status_t abi() const noexcept { return to_abi_status(code_); }

    error_code with_detail(uint32_t d) const noexcept { return error_code(code_, d); }

    friend bool operator==(error_code a, error_code b) noexcept {
        return a.code_ == b.code_ && a.detail_ == b.detail_;
    }
    friend bool operator!=(error_code a, error_code b) noexcept { return !(a == b); }
    friend bool operator==(error_code a, Err b) noexcept { return a.code_ == b; }
    friend bool operator==(Err a, error_code b) noexcept { return b == a; }

private:
    Err        code_;
    uint32_t   detail_;
};

/* Named constructors for the common cases. */
inline constexpr error_code ok() { return error_code(Err::ok); }
inline constexpr error_code make_error_code(Err e, uint32_t detail = 0) {
    return error_code(e, detail);
}

/* Policy helpers (feed the Phase 4 net policy and the Phase 10 agent loop):
 *   is_retryable()  — the failure is transient by nature; a retry is allowed
 *                     (the retry budget, Phase 6, decides whether it runs).
 *   is_transient()  — same classification exposed for the offline/queue path.
 */
inline bool is_retryable(error_code ec) noexcept {
    return ec.retry() == Retry::retryable;
}
inline bool is_transient(error_code ec) noexcept {
    return ec.retry() == Retry::retryable;
}

} /* namespace opencode::core */

#endif /* OPENCODE_CORE_ERROR_H */
