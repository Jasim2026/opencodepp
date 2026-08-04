/*
 * memory_test.cpp -- Phase 11 commit 1: entry model, validation, secret filter,
 *                     JSON round-trip, config parsing.
 *
 * No Store or network involved; pure unit tests on Entry + MemoryCfg.
 * Runs from the repo root.
 */
#include <cstdio>
#include <cstdlib>
#include <string>

#include "config/config.hpp"
#include "memory/entry.h"
#include "memory/session_memory.h"
#include "msg/message.h"
#include "store/mem_store.hpp"

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

    if (failures == 0) std::printf("memory_test: all OK\n");
    return failures == 0 ? 0 : 1;
}
