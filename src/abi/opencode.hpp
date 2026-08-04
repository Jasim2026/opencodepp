/*
 * opencode.hpp -- the C++ RAII wrapper over the C ABI (Phase 12 Task 3).
 *
 * `opencode::abi::Engine` owns an opencode_engine_t, is lifetime-safe and
 * move-only, and never throws across the ABI. It is the recommended host
 * surface for C++ consumers; the reference CLI and the Python/Java bindings
 * use the same ABI underneath.
 */
#ifndef OPENCODE_ABI_OPENCODE_HPP
#define OPENCODE_ABI_OPENCODE_HPP

#include <cstdint>

#include "opencode/opencode.h"

namespace opencode::abi {

class Engine {
public:
    /* Create with NULL config = defaults; see opencode.h. Throws nothing:
     * a failed create leaves a null engine (check valid()). */
    explicit Engine(const opencode_config_t* cfg = nullptr) noexcept
        : eng_(nullptr) {
        opencode_engine_create(cfg, &eng_);
    }
    ~Engine() {
        if (eng_ != nullptr) opencode_engine_destroy(eng_);
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&& o) noexcept : eng_(o.eng_) { o.eng_ = nullptr; }
    Engine& operator=(Engine&& o) noexcept {
        if (this != &o) {
            if (eng_ != nullptr) opencode_engine_destroy(eng_);
            eng_ = o.eng_;
            o.eng_ = nullptr;
        }
        return *this;
    }

    bool valid() const noexcept { return eng_ != nullptr; }
    opencode_engine_t* get() noexcept { return eng_; }

    opencode_status_t set_config(const opencode_config_t* cfg) noexcept {
        return eng_ != nullptr ? opencode_engine_set_config(eng_, cfg)
                               : OPENCODE_ERR_VALIDATION;
    }
    opencode_status_t run(const char* prompt, opencode_event_fn on_event = nullptr,
                          void* userdata = nullptr) noexcept {
        return eng_ != nullptr ? opencode_engine_run(eng_, prompt, on_event, userdata)
                               : OPENCODE_ERR_VALIDATION;
    }
    opencode_status_t drive(int32_t wait_ms) noexcept {
        return eng_ != nullptr ? opencode_engine_drive(eng_, wait_ms)
                               : OPENCODE_ERR_VALIDATION;
    }
    opencode_status_t cancel() noexcept {
        return eng_ != nullptr ? opencode_engine_cancel(eng_)
                               : OPENCODE_ERR_VALIDATION;
    }
    uint32_t metrics(opencode_metric_fn sink, void* userdata) noexcept {
        uint32_t n = 0;
        if (eng_ != nullptr) opencode_metrics_snapshot(eng_, sink, userdata, &n);
        return n;
    }
    opencode_status_t memory_write(opencode_memory_kind_t kind, const char* key,
                                   const char* value, const char* tags_json,
                                   char* out_id = nullptr,
                                   size_t out_id_cap = 0) noexcept {
        return eng_ != nullptr
                   ? opencode_memory_write(eng_, kind, key, value, tags_json,
                                           out_id, out_id_cap)
                   : OPENCODE_ERR_VALIDATION;
    }
    opencode_status_t memory_read(opencode_memory_kind_t kind,
                                  const char* keywords_json, char* out,
                                  size_t out_cap, size_t* out_len = nullptr) noexcept {
        return eng_ != nullptr
                   ? opencode_memory_read(eng_, kind, keywords_json, out,
                                          out_cap, out_len)
                   : OPENCODE_ERR_VALIDATION;
    }

private:
    opencode_engine_t* eng_;
};

} /* namespace opencode::abi */

#endif /* OPENCODE_ABI_OPENCODE_HPP */
