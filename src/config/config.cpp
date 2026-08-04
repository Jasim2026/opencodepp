/*
 * config.cpp -- see config.hpp. JSON load + validation with field paths.
 */
#include "config/config.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

using opencode::util::JVal;

namespace opencode::config {
namespace {

/* Parser context: logs warnings/errors and latches the first error. */
struct Ctx {
    core::Logger* log;
    core::error_code err = core::ok();
    bool failed() const { return !err.ok(); }

    void invalid(std::string_view path, const char* why) {
        if (err.ok()) err = core::make_error_code(core::Err::e_invalid_cfg);
        if (log) log->warn("config: invalid field", "path", path, "reason", why);
    }
};

bool get_str(const JVal& obj, std::string_view key, std::string_view path,
             std::string& out, bool require_nonempty, Ctx& c) {
    const JVal* v = obj.find(key);
    if (!v) {
        if (require_nonempty) c.invalid(path, "missing required field");
        return false;
    }
    if (v->kind != JVal::Kind::string) {
        c.invalid(path, "expected string");
        return false;
    }
    out.assign(v->str);
    if (require_nonempty && out.empty()) {
        c.invalid(path, "must not be empty");
        return false;
    }
    return true;
}

bool get_int(const JVal& obj, std::string_view key, std::string_view path,
             int64_t lo, int64_t hi, int64_t& out, Ctx& c) {
    const JVal* v = obj.find(key);
    if (!v) return false;
    if (v->kind != JVal::Kind::number) {
        c.invalid(path, "expected number");
        return false;
    }
    const double d = v->num;
    if (d != std::trunc(d) || d < static_cast<double>(lo) ||
        d > static_cast<double>(hi)) {
        c.invalid(path, "out of range");
        return false;
    }
    out = static_cast<int64_t>(d);
    return true;
}

bool get_double(const JVal& obj, std::string_view key, std::string_view path,
                double lo, double hi, double& out, Ctx& c) {
    const JVal* v = obj.find(key);
    if (!v) return false;
    if (v->kind != JVal::Kind::number) {
        c.invalid(path, "expected number");
        return false;
    }
    const double d = v->num;
    if (!(d >= lo && d <= hi)) {
        c.invalid(path, "out of range");
        return false;
    }
    out = d;
    return true;
}

bool get_bool(const JVal& obj, std::string_view key, std::string_view path,
              bool& out, Ctx& c) {
    const JVal* v = obj.find(key);
    if (!v) return false;
    if (v->kind != JVal::Kind::boolean) {
        c.invalid(path, "expected boolean");
        return false;
    }
    out = v->b;
    return true;
}

bool has_provider_id(const std::vector<ProviderCfg>& v, std::string_view id) {
    for (const ProviderCfg& p : v) {
        if (p.id == id) return true;
    }
    return false;
}

bool has_agent_id(const std::vector<AgentCfg>& v, std::string_view id) {
    for (const AgentCfg& a : v) {
        if (a.id == id) return true;
    }
    return false;
}

} /* namespace */

core::error_code load_config_json(std::string_view text, Config& out,
                                  core::Logger* log) {
    JVal root;
    size_t pos = 0;
    if (const core::error_code ec = util::parse_json(text, root, &pos);
        !ec.ok()) {
        if (log) {
            log->warn("config: json parse failed", "at",
                      static_cast<int64_t>(pos));
        }
        return core::make_error_code(core::Err::e_invalid_cfg);
    }
    if (root.kind != JVal::Kind::object) {
        if (log) log->warn("config: root must be a JSON object");
        return core::make_error_code(core::Err::e_invalid_cfg);
    }

    Ctx c{log};

    if (const JVal* schema = root.find("schema"); schema) {
        if (schema->kind != JVal::Kind::number) {
            c.invalid("schema", "expected number");
        } else if (schema->num > static_cast<double>(kConfigSchemaVersion)) {
            c.invalid("schema", "version newer than supported");
        } else if (log && schema->num <
                              static_cast<double>(kConfigSchemaVersion)) {
            log->warn("config: older schema version", "version", schema->num);
        }
    }
    if (c.failed()) return c.err;

    if (const JVal* p = root.find("providers"); p) {
        if (p->kind != JVal::Kind::array) {
            c.invalid("providers", "expected array");
        } else {
            out.providers.clear();
            for (size_t i = 0; i < p->arr.size() && !c.failed(); ++i) {
                char pathbuf[48];
                std::snprintf(pathbuf, sizeof pathbuf, "providers[%zu]", i);
                const JVal& e = p->arr[i];
                if (e.kind != JVal::Kind::object) {
                    c.invalid(pathbuf, "expected object");
                    continue;
                }
                ProviderCfg pc;
                std::string id;
                if (!get_str(e, "id", pathbuf, id, true, c)) continue;
                if (has_provider_id(out.providers, id)) {
                    c.invalid(pathbuf, "duplicate provider id");
                    continue;
                }
                pc.id = std::move(id);
                std::string tmp;
                if (get_str(e, "api_key", pathbuf, tmp, false, c)) {
                    pc.api_key = std::move(tmp);
                }
                if (get_str(e, "base_url", pathbuf, tmp, false, c)) {
                    pc.base_url = std::move(tmp);
                }
                if (get_str(e, "default_model", pathbuf, tmp, false, c)) {
                    pc.default_model = std::move(tmp);
                }
                if (!c.failed()) out.providers.push_back(std::move(pc));
            }
        }
    }
    if (c.failed()) return c.err;

    if (const JVal* p = root.find("agents"); p) {
        if (p->kind != JVal::Kind::array) {
            c.invalid("agents", "expected array");
        } else {
            out.agents.clear();
            for (size_t i = 0; i < p->arr.size() && !c.failed(); ++i) {
                char pathbuf[48];
                std::snprintf(pathbuf, sizeof pathbuf, "agents[%zu]", i);
                const JVal& e = p->arr[i];
                if (e.kind != JVal::Kind::object) {
                    c.invalid(pathbuf, "expected object");
                    continue;
                }
                AgentCfg ac;
                std::string id;
                if (!get_str(e, "id", pathbuf, id, true, c)) continue;
                if (has_agent_id(out.agents, id)) {
                    c.invalid(pathbuf, "duplicate agent id");
                    continue;
                }
                ac.id = std::move(id);
                std::string model;
                if (!get_str(e, "model", pathbuf, model, true, c)) continue;
                ac.model = std::move(model);
                int64_t mt = 0;
                if (get_int(e, "max_tokens", pathbuf, 0, 100'000'000, mt, c)) {
                    ac.max_tokens = static_cast<uint32_t>(mt);
                }
                if (!c.failed()) out.agents.push_back(std::move(ac));
            }
        }
    }
    if (c.failed()) return c.err;

    if (const JVal* n = root.find("network"); n) {
        if (n->kind != JVal::Kind::object) {
            c.invalid("network", "expected object");
        } else {
            int64_t v = 0;
            if (get_int(*n, "timeout_ms", "network.timeout_ms", 1, 3'600'000,
                        v, c)) {
                out.network.timeout_ms = static_cast<uint32_t>(v);
            }
            if (get_int(*n, "max_retries", "network.max_retries", 0, 20, v,
                        c)) {
                out.network.max_retries = static_cast<uint32_t>(v);
            }
            if (get_int(*n, "backoff_base_ms", "network.backoff_base_ms", 0,
                        60'000, v, c)) {
                out.network.backoff_base_ms = static_cast<uint32_t>(v);
            }
            if (get_int(*n, "backoff_max_ms", "network.backoff_max_ms", 0,
                        600'000, v, c)) {
                out.network.backoff_max_ms = static_cast<uint32_t>(v);
            }
            double jitter = 0;
            if (get_double(*n, "jitter", "network.jitter", 0.0, 0.99, jitter,
                           c)) {
                out.network.jitter = jitter;
            }
            if (out.network.backoff_max_ms < out.network.backoff_base_ms) {
                c.invalid("network", "backoff_max_ms < backoff_base_ms");
            }
        }
    }
    if (c.failed()) return c.err;

    if (const JVal* b = root.find("budget"); b) {
        if (b->kind != JVal::Kind::object) {
            c.invalid("budget", "expected object");
        } else {
            int64_t v = 0;
            if (get_int(*b, "max_tokens_per_task", "budget.max_tokens_per_task",
                        1'000, 10'000'000, v, c)) {
                out.budget.max_tokens_per_task = static_cast<uint32_t>(v);
            }
        }
    }
    if (c.failed()) return c.err;

    if (const JVal* m = root.find("memory"); m) {
        if (m->kind != JVal::Kind::object) {
            c.invalid("memory", "expected object");
        } else {
            int64_t v = 0;
            if (get_int(*m, "max_value_chars", "memory.max_value_chars",
                        1, 100'000, v, c)) {
                out.memory.max_value_chars = static_cast<uint32_t>(v);
            }
            if (get_int(*m, "max_key_chars", "memory.max_key_chars",
                        1, 1'000, v, c)) {
                out.memory.max_key_chars = static_cast<uint32_t>(v);
            }
            if (get_int(*m, "max_entries", "memory.max_entries",
                        1, 1'000'000, v, c)) {
                out.memory.max_entries = static_cast<uint32_t>(v);
            }
            if (get_int(*m, "max_entries_per_task", "memory.max_entries_per_task",
                        1, 1'000, v, c)) {
                out.memory.max_entries_per_task = static_cast<uint32_t>(v);
            }
            if (get_int(*m, "max_entry_tokens", "memory.max_entry_tokens",
                        1, 100'000, v, c)) {
                out.memory.max_entry_tokens = static_cast<uint32_t>(v);
            }
        }
    }
    if (c.failed()) return c.err;

    std::string tmp;
    if (get_str(root, "data_dir", "data_dir", tmp, false, c)) {
        out.data_dir = std::move(tmp);
    }
    if (c.failed()) return c.err;

    if (const JVal* cp = root.find("context_paths"); cp) {
        if (cp->kind != JVal::Kind::array) {
            c.invalid("context_paths", "expected array");
        } else {
            out.context_paths.clear();
            for (size_t i = 0; i < cp->arr.size() && !c.failed(); ++i) {
                const JVal& e = cp->arr[i];
                if (e.kind != JVal::Kind::string) {
                    c.invalid("context_paths", "expected array of strings");
                    continue;
                }
                out.context_paths.emplace_back(e.str);
            }
        }
    }
    if (c.failed()) return c.err;

    bool edge = false;
    if (get_bool(root, "edge_mode", "edge_mode", edge, c)) {
        out.edge_mode = edge;
    }
    if (c.failed()) return c.err;

    static constexpr std::string_view kKnown[] = {
        "schema", "providers", "agents",      "network", "budget", "memory",
        "data_dir", "context_paths", "edge_mode"};
    for (const auto& [k, v] : root.obj) {
        (void)v;
        bool known = false;
        for (const std::string_view kk : kKnown) {
            if (k == kk) {
                known = true;
                break;
            }
        }
        if (!known && log) log->warn("config: unknown key", "key", k);
    }

    return c.err;
}

core::error_code load_config_file(const std::string& path, Config& out,
                                  core::Logger* log) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        if (log) log->warn("config: cannot open file", "path", path);
        return core::make_error_code(core::Err::e_invalid_cfg);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in.good() && !in.eof()) {
        if (log) log->warn("config: read error", "path", path);
        return core::make_error_code(core::Err::e_invalid_cfg);
    }
    return load_config_json(ss.str(), out, log);
}

} /* namespace opencode::config */
