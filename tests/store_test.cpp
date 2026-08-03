// store_test.cpp -- Phase 3: the Store interface suite.
//
// The same suite runs against MemStore always, and against SqliteStore when
// the build has OPENCODE_USE_SQLITE=ON (a test-compile definition is set from
// the option in tests/CMakeLists.txt). Covers round-trip, ordering, delete,
// session-message-file relations, listener hooks, and a 50-message synthetic
// session that must reload byte-identical (JSON-serialization equality, which
// is deterministic per message).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "msg/message.h"
#include "msg/part.h"
#include "msg/role.h"
#include "store/mem_store.hpp"
#include "store/store.h"
#include "util/json.h"

#if OPENCODE_USE_SQLITE
#include "store/sqlite_store.hpp"
#endif

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

using opencode::msg::Finish;
using opencode::msg::FinishReason;
using opencode::msg::Message;
using opencode::msg::Part;
using opencode::msg::Reasoning;
using opencode::msg::Role;
using opencode::msg::Text;
using opencode::msg::ToolCall;
using opencode::msg::ToolResult;
using opencode::store::File;
using opencode::store::Session;
using opencode::store::Store;
using opencode::store::StoreListener;

struct CountingListener final : StoreListener {
    int sessions = 0;
    int messages = 0;
    int deletes = 0;
    int files = 0;
    void on_session_saved(const Session&) noexcept override { ++sessions; }
    void on_message_saved(const Message&) noexcept override { ++messages; }
    void on_message_deleted(std::string_view) noexcept override { ++deletes; }
    void on_file_saved(const File&) noexcept override { ++files; }
};

/* Deterministic per-message JSON: equal messages serialize identically. */
bool msgs_equal(const std::vector<Message>& a, const std::vector<Message>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const std::string sa =
            opencode::util::to_json(opencode::msg::to_json(a[i]));
        const std::string sb =
            opencode::util::to_json(opencode::msg::to_json(b[i]));
        if (sa != sb) {
            return false;
        }
    }
    return true;
}

std::vector<Message> make_session_messages(size_t n) {
    std::vector<Message> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Message m;
        m.id = "m" + std::to_string(i);
        m.session_id = "sess-persist";
        m.role = (i % 2 == 0) ? Role::user : Role::assistant;
        m.created_at = 1'700'000'000ull + i;
        m.parts.push_back(Part(Text{"part text " + std::to_string(i)}));
        if (i % 3 == 0) {
            m.parts.push_back(Part(Reasoning{"think " + std::to_string(i)}));
        }
        if (i % 5 == 0) {
            m.parts.push_back(Part(ToolCall{"tc" + std::to_string(i), "bash",
                                            "{\"cmd\":\"ls\"}", false}));
        }
        if (i % 7 == 0) {
            m.parts.push_back(Part(ToolResult{"tc" + std::to_string(i),
                                              "ok"}));
        }
        if (i == n - 1) {
            m.parts.push_back(Part(Finish{FinishReason::end_turn}));
        }
        out.push_back(std::move(m));
    }
    return out;
}

void run_common_suite(Store& s) {
    /* session save/get */
    Session sess;
    sess.id = "sess-common";
    sess.title = "t";
    sess.created_at = 42;
    sess.updated_at = 43;
    const Session got = s.session_save(sess);
    CHECK(got.id == "sess-common" && got.title == "t");
    const Session back = s.session_get("sess-common");
    CHECK(back.id == "sess-common" && back.created_at == 42 &&
          back.updated_at == 43);
    CHECK(s.session_get("no-such-session").id.empty());

    /* empty session id -> generated, non-empty */
    const Session gen = s.session_save(Session{});
    CHECK(!gen.id.empty());

    /* messages: ordering + equality */
    std::vector<Message> msgs;
    for (int i = 0; i < 3; ++i) {
        Message m;
        m.id = "cm" + std::to_string(i);
        m.session_id = "sess-common";
        m.role = i == 0 ? Role::system : (i == 1 ? Role::user : Role::assistant);
        m.parts.push_back(Part(Text{"hello " + std::to_string(i)}));
        msgs.push_back(m);
        s.message_save(m);
    }
    const std::vector<Message> loaded = s.messages_by_session("sess-common");
    CHECK(loaded.size() == 3);
    CHECK(msgs_equal(msgs, loaded));
    CHECK(loaded[0].id == "cm0" && loaded[2].id == "cm2");

    /* delete removes exactly the target */
    s.message_delete("cm1");
    CHECK(s.messages_by_session("sess-common").size() == 2);
    CHECK(s.messages_by_session("sess-common")[0].id == "cm0");

    /* relations: other sessions are isolated */
    Message other;
    other.id = "cm-other";
    other.session_id = "other-sess";
    other.role = Role::user;
    other.parts.push_back(Part(Text{"isolated"}));
    s.message_save(other);
    CHECK(s.messages_by_session("sess-common").size() == 2);
    CHECK(s.messages_by_session("other-sess").size() == 1);

    /* files: fresh save is version 1; same id bumps the version */
    File f1;
    f1.id = "cf1";
    f1.session_id = "sess-common";
    f1.path = "a/b.txt";
    f1.content = "v1";
    const File v1 = s.file_save_version(f1);
    CHECK(v1.version == 1 && v1.content == "v1");
    File f2 = f1;
    f2.content = "v2";
    const File v2 = s.file_save_version(f2);
    CHECK(v2.version == 2 && v2.content == "v2");
    CHECK(v2.path == "a/b.txt"); /* path preserved across versions */
    const std::vector<File> fls = s.files_by_session("sess-common");
    CHECK(fls.size() == 1 && fls[0].version == 2);

    /* listener hooks fire per mutation */
    CountingListener l;
    s.set_listener(&l);
    Session ls;
    ls.id = "sess-listener";
    s.session_save(ls);
    Message lm;
    lm.id = "cmL";
    lm.session_id = "sess-listener";
    lm.parts.push_back(Part(Text{"x"}));
    s.message_save(lm);
    s.message_delete("cmL");
    File lf;
    lf.id = "cfL";
    lf.session_id = "sess-listener";
    lf.path = "l.txt";
    s.file_save_version(lf);
    CHECK(l.sessions == 1 && l.messages == 1 && l.deletes == 1 &&
          l.files == 1);
    s.set_listener(nullptr);
}

void run_persist_suite(Store& s) {
    const std::vector<Message> msgs = make_session_messages(50);
    Session sess;
    sess.id = "sess-persist";
    sess.title = "fifty";
    sess.created_at = 1000;
    sess.updated_at = 1000;
    s.session_save(sess);
    for (const Message& m : msgs) s.message_save(m);

    const std::vector<Message> loaded = s.messages_by_session("sess-persist");
    CHECK(loaded.size() == 50);
    CHECK(msgs_equal(msgs, loaded));
    CHECK(loaded.front().id == "m0" && loaded.back().id == "m49");
}

void test_mem() {
    auto s = opencode::store::create_mem_store();
    run_common_suite(*s);
    run_persist_suite(*s);
}

#if OPENCODE_USE_SQLITE
void sqlite_cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void test_sqlite() {
    const std::string path = "store_test_sqlite.db";
    sqlite_cleanup(path);

    std::vector<Message> msgs;
    {
        std::unique_ptr<Store> s;
        CHECK(opencode::store::create_sqlite_store(path.c_str(), s).ok());
        run_common_suite(*s);
        msgs = make_session_messages(50);
        Session sess;
        sess.id = "sess-persist";
        sess.title = "fifty";
        sess.created_at = 1000;
        sess.updated_at = 1000;
        s->session_save(sess);
        for (const Message& m : msgs) s->message_save(m);
        File f;
        f.id = "pf1";
        f.session_id = "sess-persist";
        f.path = "x.txt";
        f.content = "one";
        f.created_at = 5;
        s->file_save_version(f);
        File f2 = f;
        f2.content = "two";
        s->file_save_version(f2);
    }
    {
        std::unique_ptr<Store> s;
        CHECK(opencode::store::create_sqlite_store(path.c_str(), s).ok());
        const Session sess = s->session_get("sess-persist");
        CHECK(sess.id == "sess-persist" && sess.title == "fifty");

        /* reload: 50 messages, byte-identical, in order */
        const std::vector<Message> loaded =
            s->messages_by_session("sess-persist");
        CHECK(loaded.size() == 50);
        CHECK(msgs_equal(msgs, loaded));
        CHECK(loaded.front().id == "m0" && loaded.back().id == "m49");

        /* file version survived the reopen */
        const std::vector<File> fls = s->files_by_session("sess-persist");
        CHECK(fls.size() == 1 && fls[0].version == 2 &&
              fls[0].content == "two" && fls[0].path == "x.txt");
    }
    sqlite_cleanup(path);
}
#endif /* OPENCODE_USE_SQLITE */
} /* namespace */

int main() {
    test_mem();
#if OPENCODE_USE_SQLITE
    test_sqlite();
#endif
    if (failures == 0) {
        std::printf("store_test: OK\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "store_test: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
