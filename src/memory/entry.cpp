/*
 * entry.cpp -- entry validation, serialization, secret filter (see entry.h).
 */
#include "memory/entry.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

#include "util/json.h"

namespace opencode::memory {

namespace {

using util::JVal;

/* Case-insensitive substring search. */
bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return false;
    auto to_lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string h(haystack.size(), '\0');
    std::transform(haystack.begin(), haystack.end(), h.begin(), to_lower);
    std::string n(needle.size(), '\0');
    std::transform(needle.begin(), needle.end(), n.begin(), to_lower);
    return h.find(n) != std::string::npos;
}

/* True when key is a valid identifier: [a-z0-9._-]{1,max}. */
bool valid_key_chars(std::string_view key, std::uint32_t max_chars) {
    if (key.empty() || key.size() > max_chars) return false;
    for (const char c : key) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

const char* kind_names[] = {"decision", "fact", "task_state",
                            "repo_rule", "lesson", "user_pref"};

} /* namespace */

std::string_view kind_name(Kind k) noexcept {
    const auto i = static_cast<std::uint8_t>(k);
    return i < 6 ? kind_names[i] : "unknown";
}

Kind kind_from_name(std::string_view s) noexcept {
    for (std::uint8_t i = 0; i < 6; ++i) {
        if (s == kind_names[i]) return static_cast<Kind>(i);
    }
    return Kind::fact; /* default */
}

bool has_secret_value(std::string_view key, std::string_view value) {
    if (contains_ci(key, "token") || contains_ci(key, "password") ||
        contains_ci(key, "secret"))
        return true;
    if (contains_ci(value, "token") || contains_ci(value, "password") ||
        contains_ci(value, "secret"))
        return true;
    /* "sk-..." prefix in key or value. */
    if (key.size() >= 3 && key[0] == 's' && key[1] == 'k' && key[2] == '-')
        return true;
    if (value.size() >= 3 && value[0] == 's' && value[1] == 'k' &&
        value[2] == '-')
        return true;
    /* "key=sk-..." anywhere in value. */
    if (value.find("key=sk-") != std::string_view::npos) return true;
    return false;
}

core::error_code validate_entry(const Entry& e, const config::MemoryCfg& caps) {
    if (!valid_key_chars(e.key, caps.max_key_chars))
        return core::make_error_code(core::Err::e_invalid_cfg);
    if (e.value.size() > caps.max_value_chars)
        return core::make_error_code(core::Err::e_invalid_cfg);
    if (has_secret_value(e.key, e.value))
        return core::make_error_code(core::Err::e_invalid_cfg);
    if (static_cast<std::uint8_t>(e.kind) >= 6)
        return core::make_error_code(core::Err::e_model_unsup);
    return core::ok();
}

std::string to_json(const Entry& e) {
    using util::JVal;
    std::vector<std::pair<std::string_view, JVal>> fields;
    fields.emplace_back("id", JVal::Str(e.id));
    fields.emplace_back("kind", JVal::Str(kind_name(e.kind)));
    fields.emplace_back("scope", JVal::Str(e.scope));
    fields.emplace_back("key", JVal::Str(e.key));
    fields.emplace_back("value", JVal::Str(e.value));
    fields.emplace_back("source", JVal::Str(e.source));
    fields.emplace_back("created_at",
                        JVal::Num(static_cast<double>(e.created_at)));
    fields.emplace_back("ttl_s", JVal::Num(static_cast<double>(e.ttl_s)));
    if (!e.tags.empty()) {
        std::vector<JVal> tags;
        tags.reserve(e.tags.size());
        for (const std::string& t : e.tags) tags.push_back(JVal::Str(t));
        fields.emplace_back("tags", JVal::Array(std::move(tags)));
    }
    return util::to_json(JVal::Object(std::move(fields)));
}

core::error_code from_json(std::string_view json, Entry& out) {
    JVal root;
    std::size_t pos = 0;
    if (const core::error_code ec = util::parse_json(json, root, &pos); !ec.ok())
        return ec;
    if (root.kind != JVal::Kind::object)
        return core::make_error_code(core::Err::e_proto_parse);

    const auto str = [&](std::string_view k) -> std::string_view {
        const JVal* v = root.find(k);
        return (v != nullptr && v->kind == JVal::Kind::string) ? v->str
                                                               : std::string_view{};
    };
    const auto num = [&](std::string_view k, double dflt = 0) -> double {
        const JVal* v = root.find(k);
        return (v != nullptr && v->kind == JVal::Kind::number) ? v->num : dflt;
    };

    out.id = std::string(str("id"));
    out.kind = kind_from_name(str("kind"));
    out.scope = std::string(str("scope"));
    out.key = std::string(str("key"));
    out.value = std::string(str("value"));
    out.source = std::string(str("source"));
    out.created_at = static_cast<std::uint64_t>(num("created_at"));
    out.ttl_s = static_cast<std::uint64_t>(num("ttl_s"));
    out.tags.clear();
    if (const JVal* t = root.find("tags");
        t != nullptr && t->kind == JVal::Kind::array) {
        out.tags.reserve(t->arr.size());
        for (const JVal& v : t->arr) {
            if (v.kind == JVal::Kind::string)
                out.tags.emplace_back(v.str);
        }
    }
    return core::ok();
}

std::string to_env_text(const Entry& e, std::uint32_t max_chars) {
    std::string text = "[" + std::string(kind_name(e.kind)) + "] " + e.key +
                       ": " + e.value;
    if (max_chars > 0 && text.size() > max_chars) text.resize(max_chars);
    return text;
}

std::string entry_id(std::string_view scope, std::string_view key) {
    /* Stable id: "mem:<scope>:<key>" - used for workspace memory where
     * the Store File row must have a stable id for upsert semantics. */
    return "mem:" + std::string(scope) + ":" + std::string(key);
}

} /* namespace opencode::memory */
