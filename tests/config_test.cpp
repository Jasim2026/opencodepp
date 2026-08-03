// config_test.cpp -- Phase 3: config load/validate, env snapshot caching.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "config/env_snapshot.hpp"
#include "core/error.h"
#include "core/log.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

struct Capture {
    std::vector<std::string> lines;
};

void sink(void* userdata, opencode::core::Level,
          std::string_view line) noexcept {
    auto* c = static_cast<Capture*>(userdata);
    c->lines.emplace_back(line);
}

bool has_line(const Capture& c, const std::string& needle) {
    for (const std::string& l : c.lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

void test_defaults() {
    using namespace opencode::config;
    Config cfg;
    const opencode::core::error_code ec =
        load_config_json("{}", cfg, nullptr);
    CHECK(ec.ok());
    CHECK(cfg.providers.empty());
    CHECK(cfg.agents.empty());
    CHECK(cfg.network.timeout_ms == 30'000);
    CHECK(cfg.network.max_retries == 3);
    CHECK(cfg.network.backoff_base_ms == 250);
    CHECK(cfg.network.backoff_max_ms == 8'000);
    CHECK(cfg.network.jitter == 0.2);
    CHECK(cfg.budget.max_tokens_per_task == 12'000);
    CHECK(cfg.data_dir.empty());
    CHECK(cfg.context_paths.empty());
    CHECK(!cfg.edge_mode);
}

void test_override() {
    using namespace opencode::config;
    const std::string json =
        R"({
          "schema": 1,
          "providers": [
            {"id": "anthropic", "api_key": "sk-test", "base_url": "https://api.anthropic.com",
             "default_model": "claude-sonnet-4-5"},
            {"id": "openai", "default_model": "gpt-4.1"}
          ],
          "agents": [
            {"id": "coder", "model": "claude-sonnet-4-5", "max_tokens": 24000}
          ],
          "network": {"timeout_ms": 5000, "max_retries": 2, "backoff_base_ms": 100,
                      "backoff_max_ms": 2000, "jitter": 0.4},
          "budget": {"max_tokens_per_task": 8000},
          "data_dir": "/var/lib/opencodepp",
          "context_paths": ["src", "include"],
          "edge_mode": true
        })";
    Config cfg;
    const opencode::core::error_code ec =
        load_config_json(json, cfg, nullptr);
    CHECK(ec.ok());
    CHECK(cfg.providers.size() == 2);
    CHECK(cfg.providers[0].id == "anthropic");
    CHECK(cfg.providers[0].api_key == "sk-test");
    CHECK(cfg.providers[0].default_model == "claude-sonnet-4-5");
    CHECK(cfg.providers[1].base_url.empty()); /* optional field absent */
    CHECK(cfg.agents.size() == 1);
    CHECK(cfg.agents[0].id == "coder");
    CHECK(cfg.agents[0].model == "claude-sonnet-4-5");
    CHECK(cfg.agents[0].max_tokens == 24'000);
    CHECK(cfg.network.timeout_ms == 5'000);
    CHECK(cfg.network.max_retries == 2);
    CHECK(cfg.network.jitter == 0.4);
    CHECK(cfg.budget.max_tokens_per_task == 8'000);
    CHECK(cfg.data_dir == "/var/lib/opencodepp");
    CHECK(cfg.context_paths.size() == 2 && cfg.context_paths[1] == "include");
    CHECK(cfg.edge_mode);
}

void test_invalid() {
    using namespace opencode::config;
    Capture cap;
    opencode::core::Logger logger(&sink, &cap, opencode::core::Level::warn);

    Config cfg;

    /* not JSON at all */
    CHECK(!load_config_json("{nope", cfg, nullptr).ok());

    /* root not an object */
    CHECK(!load_config_json("[1,2,3]", cfg, nullptr).ok());

    /* bad value type -> invalid_config with field path logged */
    CHECK(!load_config_json(R"({"network":{"timeout_ms":"fast"}})", cfg,
                            &logger)
               .ok());
    CHECK(has_line(cap, "network.timeout_ms"));

    /* out of range */
    CHECK(!load_config_json(R"({"network":{"jitter":1.5}})", cfg, nullptr)
               .ok());

    /* backoff invariant */
    CHECK(!load_config_json(
               R"({"network":{"backoff_base_ms":5000,"backoff_max_ms":100}})",
               cfg, nullptr)
               .ok());

    /* missing required provider id */
    CHECK(!load_config_json(R"({"providers":[{"api_key":"x"}]})", cfg,
                            nullptr)
               .ok());

    /* duplicate provider id */
    CHECK(!load_config_json(
               R"({"providers":[{"id":"a"},{"id":"a"}]})", cfg, nullptr)
               .ok());

    /* missing required agent model */
    CHECK(!load_config_json(R"({"agents":[{"id":"a"}]})", cfg, nullptr)
               .ok());

    /* newer schema version rejected */
    CHECK(!load_config_json(R"({"schema":99})", cfg, nullptr).ok());

    /* unknown key: warning, not error */
    Capture warn_cap;
    opencode::core::Logger warn_logger(&sink, &warn_cap,
                                       opencode::core::Level::warn);
    CHECK(load_config_json(R"({"frobnicate":true})", cfg, &warn_logger).ok());
    CHECK(has_line(warn_cap, "frobnicate"));
}

void test_file_load() {
    using namespace opencode::config;
    const std::string path = "config_test_tmp.json";
    {
        std::ofstream out(path);
        out << R"({"network":{"max_retries":5},"edge_mode":true})";
    }
    Config cfg;
    const opencode::core::error_code ec = load_config_file(path, cfg, nullptr);
    CHECK(ec.ok());
    CHECK(cfg.network.max_retries == 5);
    CHECK(cfg.edge_mode);
    std::remove(path.c_str());

    /* missing file -> error */
    CHECK(!load_config_file("no_such_config_file.json", cfg, nullptr).ok());
}

void test_env_snapshot() {
    using namespace opencode::config;
    const EnvSnapshot& a = env_snapshot();
    CHECK(!a.platform.empty());
    CHECK(!a.arch.empty());
    CHECK(!a.shell.empty());
    CHECK(a.load_avg >= 0.0);
    if (a.is_git_repo) {
        CHECK(!a.git_branch.empty());
    }

    /* cached: no rebuild between calls */
    const uint64_t builds1 = env_snapshot_build_count();
    const EnvSnapshot& b = env_snapshot();
    CHECK(&a == &b);
    CHECK(env_snapshot_build_count() == builds1);

    /* invalidate forces one rebuild */
    env_snapshot_invalidate();
    const EnvSnapshot& c = env_snapshot();
    CHECK(env_snapshot_build_count() == builds1 + 1);
    CHECK(c.platform == a.platform);
    CHECK(c.arch == a.arch);
}
} /* namespace */

int main() {
    test_defaults();
    test_override();
    test_invalid();
    test_file_load();
    test_env_snapshot();
    if (failures == 0) {
        std::printf("config_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "config_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
