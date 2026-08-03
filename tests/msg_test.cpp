// msg_test.cpp -- Phase 2: role/finish enums, part variant, message conveniences,
// size estimate, and JSON interop.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/error.h"
#include "msg/finish.h"
#include "msg/message.h"
#include "msg/part.h"
#include "msg/role.h"
#include "util/json.h"

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

void test_roles() {
    using namespace opencode::msg;
    for (const Role r : {Role::user, Role::assistant, Role::system, Role::tool}) {
        const std::optional<Role> back = from_string<Role>(to_string(r));
        CHECK(back.has_value() && *back == r);
    }
    CHECK(!from_string<Role>("admin").has_value());
    CHECK(!from_string<Role>("User").has_value()); /* case-sensitive */
    CHECK(from_string<Role>("tool").value() == Role::tool);
}

void test_finish() {
    using namespace opencode::msg;
    for (const FinishReason r : {FinishReason::end_turn, FinishReason::max_tokens,
                                 FinishReason::tool_use, FinishReason::canceled,
                                 FinishReason::error, FinishReason::permission_denied,
                                 FinishReason::unknown}) {
        const std::optional<FinishReason> back =
            from_string<FinishReason>(to_string(r));
        CHECK(back.has_value() && *back == r);
    }
    CHECK(!from_string<FinishReason>("stop").has_value());
}

void test_part_variant() {
    using namespace opencode::msg;
    Part p = Text{"hello world"};
    CHECK(part_kind(p) == PartKind::text);
    CHECK(holds<Text>(p));
    CHECK(as<Text>(p) != nullptr);
    CHECK(as<ToolCall>(p) == nullptr);

    const Text* t = as<Text>(p);
    CHECK(t != nullptr && t->content == "hello world");

    visit([](const auto&) {}, p); /* generic visitor compiles */

    Part tc = ToolCall{"c1", "bash", "{\"cmd\":\"ls\"}", false};
    CHECK(part_kind(tc) == PartKind::tool_call);
    const ToolCall* c = as<ToolCall>(tc);
    CHECK(c != nullptr && c->id == "c1" && c->name == "bash");

    Part fin = Finish{FinishReason::tool_use};
    CHECK(part_kind(fin) == PartKind::finish);
    CHECK(as<Finish>(fin)->reason == FinishReason::tool_use);
}

void test_size_estimate() {
    using namespace opencode::msg;
    const size_t small = part_size_estimate(Part(Text{"a"}));
    const size_t big = part_size_estimate(Part(Text{std::string(1000, 'x')}));
    CHECK(big > small);
    CHECK(big - small >= 900);

    const size_t tc = part_size_estimate(Part(ToolCall{"id", "tool", "{}", false}));
    const size_t fr = part_size_estimate(Part(Finish{FinishReason::end_turn}));
    CHECK(tc > fr); /* more fields */

    const size_t bin =
        part_size_estimate(Part(Binary{"image/png", std::vector<std::uint8_t>(64)}));
    CHECK(bin > fr);
}

void test_message_conveniences() {
    using namespace opencode::msg;
    Message m;
    m.id = "m1";
    m.role = Role::assistant;
    m.parts.push_back(Text{"Hello "});
    m.parts.push_back(ToolCall{"c1", "bash", "{\"cmd\":\"ls\"}", false});
    m.parts.push_back(Text{"world"});
    m.parts.push_back(ToolResult{"c1", "ok", false});

    CHECK(!m.is_finished());
    CHECK(m.finish_reason() == FinishReason::unknown);

    m.parts.push_back(Finish{FinishReason::tool_use});
    CHECK(m.is_finished());
    CHECK(m.finish_reason() == FinishReason::tool_use);

    CHECK(m.content_text() == "Hello world");
    const std::vector<const ToolCall*> calls = m.tool_calls();
    CHECK(calls.size() == 1 && calls[0]->id == "c1");
    const std::vector<const ToolResult*> results = m.tool_results();
    CHECK(results.size() == 1 && results[0]->call_id == "c1" &&
          results[0]->is_error == false);
}

void test_json_round_trip() {
    using namespace opencode::core;
    using namespace opencode::util;
    using namespace opencode::msg;

    Message m;
    m.id = "m42";
    m.session_id = "s7";
    m.role = Role::tool;
    m.model = "gpt-4";
    m.created_at = 1234567890ull;
    m.parts.push_back(Text{"hello"});
    m.parts.push_back(ImageUrl{"https://example.com/a.png"});
    m.parts.push_back(Binary{"image/png", {0x89, 0x50, 0x4e, 0x47, 0x00}});
    m.parts.push_back(ToolCall{"c1", "bash", "{\"cmd\":\"ls\"}", true});
    m.parts.push_back(ToolResult{"c1", "ok", true});
    m.parts.push_back(Finish{FinishReason::permission_denied});

    /* to_json -> serialize -> parse -> from_json */
    const std::string wire = to_json(to_json(m));
    JVal root;
    const error_code ec = parse_json(wire, root);
    CHECK(ec.ok());
    Message m2;
    const error_code ec2 = from_json(root, m2);
    CHECK(ec2.ok());

    CHECK(m2.id == m.id);
    CHECK(m2.session_id == m.session_id);
    CHECK(m2.role == m.role);
    CHECK(m2.model == m.model);
    CHECK(m2.created_at == m.created_at);
    CHECK(m2.parts.size() == m.parts.size());
    CHECK(part_kind(m2.parts[0]) == PartKind::text &&
          as<Text>(m2.parts[0])->content == "hello");
    CHECK(part_kind(m2.parts[1]) == PartKind::image_url &&
          as<ImageUrl>(m2.parts[1])->url == "https://example.com/a.png");
    const Binary* b = as<Binary>(m2.parts[2]);
    CHECK(b != nullptr && b->mime == "image/png" &&
          b->data == std::vector<std::uint8_t>({0x89, 0x50, 0x4e, 0x47, 0x00}));
    const ToolCall* tc = as<ToolCall>(m2.parts[3]);
    CHECK(tc != nullptr && tc->id == "c1" && tc->name == "bash" && tc->finished);
    const ToolResult* tr = as<ToolResult>(m2.parts[4]);
    CHECK(tr != nullptr && tr->call_id == "c1" && tr->is_error);
    const Finish* fr = as<Finish>(m2.parts[5]);
    CHECK(fr != nullptr && fr->reason == FinishReason::permission_denied);

    /* unknown kind -> error */
    JVal bad = to_json(m);
    const JVal* parts = bad.find("parts");
    const_cast<JVal*>(parts)->arr[0] =
        JVal::Object({{"kind", JVal::Str("mystery")}});
    Message m3;
    CHECK(!from_json(bad, m3).ok());
    /* missing parts array -> error */
    JVal noparts = to_json(m);
    JVal noarr = noparts;
    /* rebuild without parts: clone object minus parts */
    std::vector<std::pair<std::string_view, JVal>> f;
    for (auto& [k, v] : noarr.obj) {
        if (k != "parts") f.emplace_back(k, std::move(v));
    }
    JVal obj = JVal::Object(std::move(f));
    CHECK(!from_json(obj, m3).ok());
}
} /* namespace */

int main() {
    test_roles();
    test_finish();
    test_part_variant();
    test_size_estimate();
    test_message_conveniences();
    test_json_round_trip();
    if (failures == 0) {
        std::printf("msg_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "msg_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
