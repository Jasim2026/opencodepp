/*
 * memory_test.cpp -- Phase 11 commit 1: entry model, validation, secret filter,
 *                     JSON round-trip, config parsing.
 *
 * No Store or network involved; pure unit tests on Entry + MemoryCfg.
 * Runs from the repo root.
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "memory/entry.h"
#include "memory/session_memory.h"
#include "memory/workspace_memory.h"
#include "msg/message.h"
#include "store/mem_store.hpp"
#include "tools/memory_tool.h"
#include "tools/registry.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)
} /* namespace */

int main() {
    opencode::config::MemoryCfg caps;

    /* ---- secret filter ---- */
    CHECK(opencode::memory::has_secret_value("api_key", "sk-abc123"));
    CHECK(opencode::memory::has_secret_value("token", "value"));
    CHECK(opencode::memory::has_secret_value("my password", "value"));
    CHECK(opencode::memory::has_secret_value("secret_field", "x"));
    CHECK(opencode::memory::has_secret_value("key", "key=sk-xyz"));
    CHECK(!opencode::memory::has_secret_value("path", "/tmp/a.txt"));
    CHECK(!opencode::memory::has_secret_value("name", "hello world"));

    /* ---- validation: key charset ---- */
    {
        opencode::memory::Entry e;
        e.key = "good-key_0.1";
        e.value = "v";
        CHECK(opencode::memory::validate_entry(e, caps).ok());
    }
    {
        opencode::memory::Entry e;
        e.key = "bad key space";
        e.value = "v";
        CHECK(!opencode::memory::validate_entry(e, caps).ok());
    }
    {
        opencode::memory::Entry e;
        e.key = "";
        e.value = "v";
        CHECK(!opencode::memory::validate_entry(e, caps).ok());
    }

    /* ---- validation: key length cap ---- */
    {
        opencode::memory::Entry e;
        e.key = std::string(65, 'a');
        e.value = "v";
        CHECK(!opencode::memory::validate_entry(e, caps).ok());
    }

    /* ---- validation: value length cap ---- */
    {
        opencode::memory::Entry e;
        e.key = "k";
        e.value = std::string(513, 'x');
        CHECK(!opencode::memory::validate_entry(e, caps).ok());
    }
    {
        opencode::memory::Entry e;
        e.key = "k";
        e.value = std::string(512, 'x');
        CHECK(opencode::memory::validate_entry(e, caps).ok());
    }

    /* ---- validation: secret detection ---- */
    {
        opencode::memory::Entry e;
        e.key = "api_key";
        e.value = "sk-abc123";
        CHECK(!opencode::memory::validate_entry(e, caps).ok());
    }

    /* ---- kind round-trip ---- */
    {
        using opencode::memory::Kind;
        using opencode::memory::kind_from_name;
        using opencode::memory::kind_name;
        CHECK(kind_from_name("decision") == Kind::decision);
        CHECK(kind_from_name("lesson") == Kind::lesson);
        CHECK(kind_from_name("user_pref") == Kind::user_pref);
        CHECK(kind_name(Kind::fact) == "fact");
        CHECK(kind_name(Kind::repo_rule) == "repo_rule");
    }

    /* ---- JSON round-trip ---- */
    {
        opencode::memory::Entry e;
        e.id = "mem:ws:test-key";
        e.kind = opencode::memory::Kind::lesson;
        e.scope = "workspace";
        e.key = "test-key";
        e.value = "always build before commit";
        e.source = "turn-3";
        e.created_at = 1700000000;
        e.ttl_s = 0;
        e.tags = {"build", "ci"};

        const std::string json = opencode::memory::to_json(e);
        CHECK(!json.empty());

        opencode::memory::Entry e2;
        CHECK(opencode::memory::from_json(json, e2).ok());
        CHECK(e2.id == e.id);
        CHECK(e2.kind == e.kind);
        CHECK(e2.scope == e.scope);
        CHECK(e2.key == e.key);
        CHECK(e2.value == e.value);
        CHECK(e2.source == e.source);
        CHECK(e2.created_at == e.created_at);
        CHECK(e2.ttl_s == e.ttl_s);
        CHECK(e2.tags.size() == 2);
        CHECK(e2.tags[0] == "build");
        CHECK(e2.tags[1] == "ci");
    }

    /* ---- to_env_text ---- */
    {
        opencode::memory::Entry e;
        e.kind = opencode::memory::Kind::fact;
        e.key = "build_tool";
        e.value = "cmake";
        const std::string text = opencode::memory::to_env_text(e);
        CHECK(text.find("[fact]") != std::string::npos);
        CHECK(text.find("build_tool") != std::string::npos);
        CHECK(text.find("cmake") != std::string::npos);
    }

    /* ---- to_env_text truncation ---- */
    {
        opencode::memory::Entry e;
        e.kind = opencode::memory::Kind::fact;
        e.key = "k";
        e.value = std::string(200, 'v');
        const std::string text = opencode::memory::to_env_text(e, 30);
        CHECK(text.size() <= 30);
    }

    /* ---- entry_id deterministic ---- */
    {
        const std::string a = opencode::memory::entry_id("ws", "foo");
        const std::string b = opencode::memory::entry_id("ws", "foo");
        CHECK(a == b);
        CHECK(a == "mem:ws:foo");
    }

    /* ---- config memory section ---- */
    {
        opencode::config::Config cfg;
        const std::string json = R"({
            "schema": 1,
            "providers": [{"id":"p","base_url":"http://x","api_key":"k"}],
            "agents": [{"id":"a","model":"m"}],
            "memory": {"max_value_chars": 1024, "max_entries": 100}
        })";
        CHECK(opencode::config::load_config_json(json, cfg).ok());
        CHECK(cfg.memory.max_value_chars == 1024);
        CHECK(cfg.memory.max_entries == 100);
        CHECK(cfg.memory.max_key_chars == 64);  /* default */
    }

    /* ---- checkpoint / resume round-trip ---- */
    {
        auto mem = opencode::store::create_mem_store();
        std::string sess = "sess-test-1";

        std::vector<opencode::msg::Message> msgs;
        {
            opencode::msg::Message u;
            u.id = "m1";
            u.session_id = sess;
            u.role = opencode::msg::Role::user;
            u.parts.push_back(opencode::msg::Text{"add a license"});
            msgs.push_back(std::move(u));
        }
        {
            opencode::msg::Message a;
            a.id = "m2";
            a.session_id = sess;
            a.role = opencode::msg::Role::assistant;
            a.parts.push_back(opencode::msg::Text{"done"});
            msgs.push_back(std::move(a));
        }
        const std::vector<std::string> edits = {"LICENSE (file.write)"};

        const std::string saved = opencode::memory::checkpoint(
            mem.get(), sess, msgs, 42, edits, "hash-abc");
        CHECK(saved == sess);

        const std::string ws = "/tmp/opencode_mem_ws1";
        ::system(("rm -rf " + ws + " && mkdir -p " + ws).c_str());

        /* resume with a matching workspace hash */
        std::string h1;
        CHECK(opencode::memory::workspace_hash(ws, h1).ok());
        CHECK(opencode::memory::workspace_hash(ws, h1).ok());
        opencode::memory::SessionCheckpoint r = opencode::memory::resume(
            mem.get(), sess, ws);
        CHECK(r.found);
        CHECK(r.messages.size() == 2);
        CHECK(r.messages[0].role == opencode::msg::Role::user);
        CHECK(r.messages[1].content_text() == "done");
        CHECK(r.tokens_used == 42);
        CHECK(r.applied_edits.size() == 1);
        CHECK(r.applied_edits[0] == "LICENSE (file.write)");
        CHECK(!r.workspace_hash.empty());
    }

    /* ---- resume workspace-hash mismatch ---- */
    {
        auto mem = opencode::store::create_mem_store();
        std::string sess = "sess-test-2";

        std::vector<opencode::msg::Message> msgs;
        opencode::msg::Message u;
        u.id = "m1";
        u.session_id = sess;
        u.role = opencode::msg::Role::user;
        u.parts.push_back(opencode::msg::Text{"hello"});
        msgs.push_back(std::move(u));

        /* hash an empty dir, then add a file and re-check */
        std::string ws = "/tmp/opencode_mem_ws2";
        ::system(("rm -rf " + ws + " && mkdir -p " + ws).c_str());
        std::string h1;
        CHECK(opencode::memory::workspace_hash(ws, h1).ok());
        CHECK(opencode::memory::checkpoint(mem.get(), sess, msgs, 5, {}, h1) ==
              sess);

        /* workspace changed while offline: hash must differ */
        ::system(("echo 'x' > " + ws + "/extra.txt").c_str());
        opencode::memory::SessionCheckpoint r =
            opencode::memory::resume(mem.get(), sess, ws);
        CHECK(r.found);
        CHECK(!r.workspace_matches);
        CHECK(r.messages.size() == 1);
    }

    /* ---- workspace_hash determinism + change sensitivity ---- */
    {
        std::string ws = "/tmp/opencode_mem_ws3";
        ::system(("rm -rf " + ws + " && mkdir -p " + ws).c_str());
        std::string h1, h2;
        CHECK(opencode::memory::workspace_hash(ws, h1).ok());
        CHECK(opencode::memory::workspace_hash(ws, h2).ok());
        CHECK(h1 == h2);
        CHECK(!h1.empty());

        ::system(("printf 'a\\n' > " + ws + "/a.txt").c_str());
        ::system(("printf 'b\\n' > " + ws + "/b.txt").c_str());
        std::string h3;
        CHECK(opencode::memory::workspace_hash(ws, h3).ok());
        CHECK(h3 != h1);

        /* order-independence: re-write the same bytes */
        ::system(("printf 'b\\n' > " + ws + "/b.txt").c_str());
        std::string h4;
        CHECK(opencode::memory::workspace_hash(ws, h4).ok());
        CHECK(h4 == h3);
    }

    /* ---- checkpoint is a no-op without a store ---- */
    {
        const std::string saved = opencode::memory::checkpoint(
            nullptr, "sess-x", {}, 0, {}, "");
        CHECK(saved.empty());
        opencode::memory::SessionCheckpoint r =
            opencode::memory::resume(nullptr, "sess-x", "/tmp");
        CHECK(!r.found);
    }

    /* ---- workspace write/read scoping ---- */
    {
        auto mem = opencode::store::create_mem_store();
        std::string ws = "/tmp/mem-ws-a";
        ::system(("rm -rf " + ws + " && mkdir -p " + ws).c_str());

        opencode::memory::Entry e;
        e.kind = opencode::memory::Kind::fact;
        e.key = "build_tool";
        e.value = "cmake + ninja";
        e.source = "test";
        e.created_at = 100;

        std::string id;
        CHECK(opencode::memory::write_entry(mem.get(), ws, e, caps, &id).ok());
        CHECK(id == opencode::memory::entry_id(
                        opencode::memory::workspace_scope(ws), "build_tool"));

        auto all = opencode::memory::read_entries(mem.get(), ws, caps);
        CHECK(all.size() == 1);
        CHECK(all[0].key == "build_tool");
        CHECK(all[0].value == "cmake + ninja");
        CHECK(all[0].scope == opencode::memory::workspace_scope(ws));

        /* other workspace roots see nothing */
        CHECK(opencode::memory::read_entries(mem.get(), "/tmp/mem-ws-other",
                                             caps)
                  .empty());

        /* kind filter */
        CHECK(opencode::memory::read_entries(mem.get(), ws, caps,
                                             opencode::memory::Kind::decision)
                  .empty());
        CHECK(opencode::memory::read_entries(mem.get(), ws, caps,
                                             opencode::memory::Kind::fact)
                  .size() == 1);
    }

    /* ---- write_entry upsert + secret filter + caps ---- */
    {
        auto mem = opencode::store::create_mem_store();
        std::string ws = "/tmp/mem-ws-b";
        ::system(("rm -rf " + ws + " && mkdir -p " + ws).c_str());

        opencode::memory::Entry e;
        e.kind = opencode::memory::Kind::fact;
        e.key = "key1";
        e.value = "v1";
        e.created_at = 1;
        CHECK(opencode::memory::write_entry(mem.get(), ws, e, caps).ok());

        /* same key overwrites (upsert), count stays 1 */
        e.value = "v2";
        CHECK(opencode::memory::write_entry(mem.get(), ws, e, caps).ok());
        auto all = opencode::memory::read_entries(mem.get(), ws, caps);
        CHECK(all.size() == 1);
        CHECK(all[0].value == "v2");

        /* secret value is rejected */
        e.key = "sk-bad";
        e.value = "sk-abc123";
        CHECK(!opencode::memory::write_entry(mem.get(), ws, e, caps).ok());
        /* token-in-value rejected */
        e.key = "ok-key";
        e.value = "token=abc";
        CHECK(!opencode::memory::write_entry(mem.get(), ws, e, caps).ok());

        /* per-scope cap: ring buffer keeps max_entries rows */
        opencode::config::MemoryCfg small = caps;
        small.max_entries = 3;
        for (std::uint32_t i = 0; i < 5; ++i) {
            opencode::memory::Entry k;
            k.kind = opencode::memory::Kind::fact;
            k.key = "slot" + std::to_string(i);
            k.value = "v" + std::to_string(i);
            k.created_at = i;
            CHECK(opencode::memory::write_entry(mem.get(), ws, k, small).ok());
        }
        auto ring = opencode::memory::read_entries(mem.get(), ws, small);
        CHECK(ring.size() == small.max_entries);
    }

    /* ---- match_entries keywords ---- */
    {
        auto mem = opencode::store::create_mem_store();
        std::string ws = "/tmp/mem-ws-c";
        ::system(("rm -rf " + ws + " && mkdir -p " + ws).c_str());

        opencode::memory::Entry e;
        e.kind = opencode::memory::Kind::fact;
        e.key = "build_tool";
        e.value = "cmake ninja";
        e.created_at = 1;
        CHECK(opencode::memory::write_entry(mem.get(), ws, e, caps).ok());

        /* keyword hits key or value, case-insensitive */
        CHECK(opencode::memory::match_entries(mem.get(), ws, {"CMAKE"}, caps)
                  .size() == 1);
        CHECK(opencode::memory::match_entries(mem.get(), ws, {"build"}, caps)
                  .size() == 1);
        CHECK(opencode::memory::match_entries(mem.get(), ws, {"python"}, caps)
                  .empty());
        CHECK(opencode::memory::match_entries(mem.get(), ws, {}, caps).empty());
    }

    /* ---- entries_to_context budget ---- */
    {
        std::vector<opencode::memory::Entry> es;
        for (std::uint32_t i = 0; i < 10; ++i) {
            opencode::memory::Entry e;
            e.kind = opencode::memory::Kind::fact;
            e.key = "k" + std::to_string(i);
            e.value = std::string("value ") + std::to_string(i);
            e.created_at = i;
            es.push_back(std::move(e));
        }
        std::string ctx = opencode::memory::entries_to_context(es, 6, 0, 0);
        CHECK(ctx.find("k0") != std::string::npos);
        CHECK(ctx.find("k5") != std::string::npos);
        CHECK(ctx.find("k6") == std::string::npos);
        /* token budget: a 1-token cap admits at most a strict prefix, never
         * the whole set */
        std::string tiny = opencode::memory::entries_to_context(es, 0, 1, 0);
        CHECK(tiny.size() < ctx.size());
        /* no caps -> everything */
        CHECK(opencode::memory::entries_to_context(es, 0, 0, 0)
                  .find("k9") != std::string::npos);
    }

    /* ---- keywords_from_text ---- */
    {
        auto kw = opencode::memory::keywords_from_text(
            "Please make the build use CMake please.");
        CHECK(std::find(kw.begin(), kw.end(), "cmake") != kw.end());
        CHECK(std::find(kw.begin(), kw.end(), "please") == kw.end());
        CHECK(std::find(kw.begin(), kw.end(), "make") == kw.end());
    }

    /* ---- memory.write tool ---- */
    {
        auto mem = opencode::store::create_mem_store();
        std::string ws = "/tmp/mem-ws-tool";
        ::system(("rm -rf " + ws + " && mkdir -p " + ws).c_str());

        std::vector<std::unique_ptr<opencode::tools::Tool>> tls;
        CHECK(opencode::tools::make_memory_tools(mem.get(), ws, caps, tls).ok());
        CHECK(tls.size() == 1);
        opencode::tools::ToolRegistry reg;
        CHECK(reg.add(std::move(tls[0])).ok());

        opencode::tools::Invocation inv;
        inv.tool_name = "memory.write";
        inv.args_json =
            R"({"key":"repo_note","value":"uses cmake","kind":"fact"})";
        opencode::tools::ToolContext ctx;
        const opencode::tools::ToolResult r = reg.run("memory.write", inv, ctx);
        CHECK(r.status == opencode::tools::ToolStatus::ok);
        CHECK(r.content.find("stored") != std::string::npos);

        auto all = opencode::memory::read_entries(mem.get(), ws, caps);
        CHECK(all.size() == 1);
        CHECK(all[0].key == "repo_note");
        CHECK(all[0].value == "uses cmake");

        /* secret value is rejected by the tool */
        opencode::tools::Invocation bad;
        bad.tool_name = "memory.write";
        bad.args_json =
            R"({"key":"x","value":"sk-shhhhh","kind":"fact"})";
        const opencode::tools::ToolResult rb = reg.run("memory.write", bad, ctx);
        CHECK(rb.status == opencode::tools::ToolStatus::error);
    }

    /* ---- register_defaults includes memory.write with a store ---- */
    {
        auto mem = opencode::store::create_mem_store();
        opencode::tools::ToolRegistry reg;
        opencode::tools::RegistryOptions opts;
        opts.workspace = "/tmp/mem-ws-reg";
        opts.store = mem.get();
        CHECK(opencode::tools::register_defaults(reg, opts).ok());
        CHECK(reg.find("memory.write") != nullptr);
    }

    if (failures == 0) std::printf("memory_test: all OK\n");
    return failures == 0 ? 0 : 1;
}
