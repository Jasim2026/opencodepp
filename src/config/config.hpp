/*
 * config.hpp -- validated configuration surface for the engine.
 *
 * A Config is loaded from a JSON string (host-provided) or a file. Parsing is
 * strict about the values it reads (bad values -> Err::e_invalid_cfg with the
 * offending field path logged), lenient about the keys it does not know
 * (unknown keys -> warning, not error) so newer/older configs stay loadable.
 * All numbers are range-checked; defaults come from the struct initializers,
 * so "{}" yields a fully usable configuration.
 *
 * The schema is versioned (kConfigSchemaVersion). A file declaring a newer
 * schema version is rejected; an older one is accepted.
 */
#ifndef OPENCODE_CONFIG_CONFIG_HPP
#define OPENCODE_CONFIG_CONFIG_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "core/error.h"
#include "core/log.h"
#include "util/json.h"

namespace opencode::config {

inline constexpr uint32_t kConfigSchemaVersion = 1;

struct ProviderCfg {
    std::string id;
    std::string api_key;
    std::string base_url;
    std::string default_model;
};

struct AgentCfg {
    std::string id;
    std::string model;
    uint32_t max_tokens = 0; /* per-agent cap override; 0 = use budget default */
};

struct NetworkCfg {
    uint32_t timeout_ms = 30'000;
    uint32_t max_retries = 3;
    uint32_t backoff_base_ms = 250;
    uint32_t backoff_max_ms = 8'000;
    double jitter = 0.2; /* [0, 1): randomized backoff spread */
};

struct BudgetCfg {
    uint32_t max_tokens_per_task = 12'000;
};

/* Phase 11 memory limits. Memory is a curated list, not a database: entries
 * are capped in size and count, and only a small budgeted set is injected
 * into any one context plan. */
struct MemoryCfg {
    uint32_t max_value_chars = 512;   /* per-entry value cap                */
    uint32_t max_key_chars = 64;      /* per-entry key cap                  */
    uint32_t max_entries = 64;        /* per-scope entry count cap          */
    uint32_t max_entries_per_task = 6;   /* Tier-2 injection cap (T1)        */
    uint32_t max_entry_tokens = 400;     /* Tier-2 injection token cap (T1)  */
};

struct Config {
    std::vector<ProviderCfg> providers;
    std::vector<AgentCfg> agents;
    NetworkCfg network;
    BudgetCfg budget;
    MemoryCfg memory;
    std::string data_dir;
    std::vector<std::string> context_paths;
    bool edge_mode = false;
};

/* Parse + validate a JSON config. Unknown keys are logged as warnings (when a
 * Logger is provided) and skipped; bad values fail with Err::e_invalid_cfg
 * (ABI OPENCODE_ERR_VALIDATION). Never throws. */
core::error_code load_config_json(std::string_view text, Config& out,
                                  core::Logger* log = nullptr);

/* Read a config file and load_config_json it. e_invalid_cfg on unreadable
 * file or parse/validation failure. Never throws. */
core::error_code load_config_file(const std::string& path, Config& out,
                                  core::Logger* log = nullptr);

} /* namespace opencode::config */

#endif /* OPENCODE_CONFIG_CONFIG_HPP */
