/*
 * schema.cpp -- JSON-Schema + provider-native tools projection (Phase 8).
 *
 * The emission order of object keys is deliberate: it matches the golden
 * request fixtures in tests/fixtures/responses/ byte-for-byte (see tools_test
 * schema suites). util::JVal keeps key order via its vector-of-pairs storage.
 */
#include "tools/schema.h"

#include <utility>
#include <vector>

#include "util/json.h"

namespace opencode::tools::schema {

using opencode::util::JVal;

namespace {

std::string_view type_name(ParamType t) noexcept {
    switch (t) {
        case ParamType::string: return "string";
        case ParamType::integer: return "integer";
        case ParamType::number: return "number";
        case ParamType::boolean: return "boolean";
        case ParamType::string_array: return "array";
        case ParamType::object: return "object";
    }
    return "string";
}

JVal type_schema(ParamType t) {
    if (t == ParamType::string_array) {
        return JVal::Object(
            {{"type", JVal::Str("array")},
             {"items", JVal::Object({{"type", JVal::Str("string")}})}});
    }
    return JVal::Object({{"type", JVal::Str(type_name(t))}});
}

/* Empty string when `json` fails to parse (caller then omits the key). */
bool parse_optional(const std::string& json, JVal& out) {
    if (json.empty()) return false;
    return parse_json(json, out).ok();
}

} /* namespace */

ToolSpec make_spec(std::string id, std::string description,
                   std::vector<ParamSpec> params, bool is_read_only,
                   ToolCategory category) {
    ToolSpec s;
    s.id = id;
    s.name = id; /* the wire name == the stable engine id */
    s.description = std::move(description);
    s.params = std::move(params);
    s.params_schema = params_schema(s.params);
    s.is_read_only = is_read_only;
    s.category = category;
    return s;
}

std::string params_schema(const std::vector<ParamSpec>& params) {
    std::vector<std::pair<std::string_view, JVal>> props;
    bool any_optional = false;
    for (const ParamSpec& p : params) {
        JVal s = type_schema(p.type);
        if (!p.description.empty())
            s.obj.emplace_back("description", JVal::Str(p.description));
        JVal dv;
        if (parse_optional(p.default_json, dv))
            s.obj.emplace_back("default", std::move(dv));
        if (!p.enum_values.empty()) {
            std::vector<JVal> ev;
            ev.reserve(p.enum_values.size());
            for (const std::string& e : p.enum_values) ev.push_back(JVal::Str(e));
            s.obj.emplace_back("enum", JVal::Array(std::move(ev)));
        }
        props.emplace_back(p.name, std::move(s));
        if (!p.required) any_optional = true;
    }
    std::vector<std::pair<std::string_view, JVal>> root;
    root.emplace_back("type", JVal::Str("object"));
    if (!props.empty())
        root.emplace_back("properties", JVal::Object(std::move(props)));
    if (any_optional) {
        std::vector<JVal> req;
        for (const ParamSpec& p : params)
            if (p.required) req.push_back(JVal::Str(p.name));
        if (!req.empty()) root.emplace_back("required", JVal::Array(std::move(req)));
    }
    return util::to_json(JVal::Object(std::move(root)));
}

std::string tools_json(const std::vector<ToolSpec>& tools,
                       std::string_view wire_family) {
    if (wire_family == "anthropic") {
        std::vector<JVal> list;
        list.reserve(tools.size());
        for (const ToolSpec& t : tools) {
            JVal schema;
            (void)parse_optional(t.params_schema, schema);
            if (schema.kind != JVal::Kind::object) schema = JVal::Object({});
            list.push_back(JVal::Object(
                {{"name", JVal::Str(t.name)},
                 {"description", JVal::Str(t.description)},
                 {"input_schema", std::move(schema)}}));
        }
        return util::to_json(JVal::Array(std::move(list)));
    }
    if (wire_family == "google") {
        std::vector<JVal> decls;
        decls.reserve(tools.size());
        for (const ToolSpec& t : tools) {
            JVal params;
            (void)parse_optional(t.params_schema, params);
            if (params.kind != JVal::Kind::object) params = JVal::Object({});
            decls.push_back(JVal::Object(
                {{"name", JVal::Str(t.name)},
                 {"description", JVal::Str(t.description)},
                 {"parameters", std::move(params)}}));
        }
        return util::to_json(JVal::Array(std::vector<JVal>{
            JVal::Object({{"functionDeclarations", JVal::Array(std::move(decls))}})}));
    }
    /* default: openai family (incl. openai_compat) */
    std::vector<JVal> list;
    list.reserve(tools.size());
    for (const ToolSpec& t : tools) {
        JVal params;
        (void)parse_optional(t.params_schema, params);
        if (params.kind != JVal::Kind::object) params = JVal::Object({});
        list.push_back(JVal::Object(
            {{"type", JVal::Str("function")},
             {"function",
              JVal::Object({{"name", JVal::Str(t.name)},
                            {"description", JVal::Str(t.description)},
                            {"parameters", std::move(params)}})}}));
    }
    return util::to_json(JVal::Array(std::move(list)));
}

} /* namespace opencode::tools::schema */
