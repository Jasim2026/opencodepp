/*
 * tool.h -- the tool runtime's data model (Phase 8).
 *
 * Everything is declarative: a ToolSpec carries a JSON-Schema description of
 * its arguments (generated from ParamSpecs in schema.cpp), so the same spec
 * projects onto any provider's native tool-calling wire format. A tool only
 * returns ToolResult; cancellation and streaming progress travel in
 * ToolContext. No function here throws.
 *
 * is_read_only is truthful per tool and drives the Phase 9 gate: read-only
 * tools are safe to retry/parallelize; write tools are gated + serialized.
 */
#ifndef OPENCODE_TOOLS_TOOL_H
#define OPENCODE_TOOLS_TOOL_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"

namespace opencode::tools {

enum class ToolCategory : uint8_t {
    read = 0,      /* file.read, dir.list, file.stat, file.search   */
    write = 1,     /* file.write, file.patch                        */
    workspace = 2, /* workspace.info, git.*                         */
    graph = 3,     /* sym.* (Phase 7 symbol index)                  */
    shell = 4,     /* shell.run                                     */
};

enum class ParamType : uint8_t {
    string = 0,
    integer = 1,
    number = 2,
    boolean = 3,
    string_array = 4,
    object = 5,
};

/* Declarative parameter description -> JSON Schema (schema.cpp). */
struct ParamSpec {
    std::string name;
    ParamType type = ParamType::string;
    std::string description;
    bool required = true;
    std::string default_json;              /* serialized default; "" = none    */
    std::vector<std::string> enum_values;  /* closed set when non-empty        */
};

struct ToolSpec {
    std::string id;                /* stable engine id, e.g. "file.read"      */
    std::string name;              /* wire name the model sees (== id)        */
    std::string description;
    std::string params_schema;     /* JSON Schema of the arguments object     */
    bool is_read_only = true;
    ToolCategory category = ToolCategory::read;
    std::vector<ParamSpec> params; /* declarative source of truth             */
};

enum class ToolStatus : uint8_t { ok = 0, error = 1, canceled = 2 };

struct ToolResult {
    std::string tool_id;              /* which tool produced this             */
    ToolStatus status = ToolStatus::ok;
    std::string content;              /* text payload returned to the model   */
    bool content_is_error = false;    /* surfaced as an error to the model    */
    std::uint32_t usage_estimate = 0; /* approximate tokens of `content`      */
};

struct Invocation {
    std::string tool_name;
    std::string args_json;
    std::uint64_t span_id = 0;
};

/* Streaming progress event. `phase` is tool-defined ("spawn"/"stdout"/...);
 * `chunk` carries streamed bytes; `percent` is -1 when indeterminate. */
struct ToolProgress {
    std::uint64_t span_id = 0;
    std::string phase;
    std::string chunk;
    int percent = -1;
};

/* Cooperative cancellation: the agent (Phase 10) owns one per span and calls
 * cancel() from any thread; long-running tools poll cancelled() at safe
 * points and roll back atomically. */
class CancellationToken {
public:
    CancellationToken() = default;
    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;

    bool cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }
    void cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
    }

private:
    std::atomic<bool> cancelled_{false};
};

using ProgressFn = void (*)(void* userdata, const ToolProgress& progress);

struct ToolContext {
    std::uint64_t span_id = 0;
    CancellationToken* cancel = nullptr;
    ProgressFn on_progress = nullptr;
    void* progress_userdata = nullptr;
};

class Tool {
public:
    virtual ~Tool() = default;
    virtual const ToolSpec& spec() const = 0;
    virtual ToolResult run(const Invocation& inv, ToolContext& ctx) = 0;
};

} /* namespace opencode::tools */

#endif /* OPENCODE_TOOLS_TOOL_H */
