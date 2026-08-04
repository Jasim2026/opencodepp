/*
 * summarizer.cpp -- lossy turn summary (see header).
 */
#include "memory/summarizer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "msg/part.h"
#include "msg/role.h"
#include "msg/tokens.h"

namespace opencode::memory {

namespace {

std::string_view role_name(msg::Role r) noexcept {
    return msg::to_string(r);
}

/* First and last non-empty lines of a text; "" when empty. */
void edge_lines(std::string_view text, std::string& first,
                std::string& last) {
    first.clear();
    last.clear();
    std::string_view t = text;
    while (!t.empty() && (t.front() == '\n' || t.front() == '\r' ||
                          t.front() == ' ' || t.front() == '\t'))
        t.remove_prefix(1);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                          t.back() == ' ' || t.back() == '\t'))
        t.remove_suffix(1);
    if (t.empty()) return;

    const std::size_t n = t.find('\n');
    first = std::string(t.substr(0, n));
    const std::size_t last_nl = t.rfind('\n');
    if (last_nl == std::string_view::npos || last_nl == 0) {
        last = std::string(t);
    } else {
        std::string_view l = t.substr(last_nl + 1);
        while (!l.empty() && (l.front() == '\r' || l.front() == ' ' ||
                              l.front() == '\t'))
            l.remove_prefix(1);
        last = std::string(l);
    }
    /* collapse a single-line body */
    if (first == last) last.clear();
}

std::string line_fragment(std::string_view text) {
    std::string first, last;
    edge_lines(text, first, last);
    std::string out = first;
    if (!last.empty()) out += " ... " + last;
    return out;
}

} /* namespace */

std::uint32_t estimate_history_tokens(const std::vector<msg::Message>& msgs) {
    std::uint64_t total = 0;
    for (const msg::Message& m : msgs) {
        total += msg::estimate_tokens(m.content_text());
        total += static_cast<std::uint64_t>(m.tool_calls().size()) * 12;
        total += static_cast<std::uint64_t>(m.tool_results().size()) * 8;
    }
    return total > 0xFFFF'FFFFu ? 0xFFFF'FFFFu
                                : static_cast<std::uint32_t>(total);
}

bool needs_fold(const std::vector<msg::Message>& msgs,
                std::uint32_t context_window, std::uint32_t max_output_tokens,
                std::size_t keep_recent) {
    if (context_window == 0) return false;
    if (msgs.size() <= keep_recent) return false;
    const std::uint64_t reserve =
        static_cast<std::uint64_t>(max_output_tokens);
    const std::uint64_t budget = reserve < context_window
                                     ? static_cast<std::uint64_t>(context_window) -
                                           reserve
                                     : 0;
    return static_cast<std::uint64_t>(estimate_history_tokens(msgs)) > budget;
}

std::string local_fold_text(const std::vector<msg::Message>& msgs) {
    std::string out;
    for (const msg::Message& m : msgs) {
        if (!out.empty()) out += '\n';
        out += '[';
        out.append(role_name(m.role));
        out += "] ";
        const std::string body = m.content_text();
        if (!body.empty()) out += line_fragment(body);
        const std::size_t ncalls = m.tool_calls().size();
        const std::size_t nres = m.tool_results().size();
        if (ncalls > 0) out += " tool_calls=" + std::to_string(ncalls);
        if (nres > 0) out += " tool_results=" + std::to_string(nres);
    }
    if (!msgs.empty()) {
        const msg::Message& newest = msgs.back();
        const std::string body = newest.content_text();
        out += "\noutcome: ";
        if (!body.empty()) {
            out += line_fragment(body);
        } else if (newest.is_finished()) {
            out.append(msg::to_string(newest.finish_reason()));
        } else {
            out += "turn complete";
        }
    }
    return out;
}

FoldResult fold_oldest(const std::vector<msg::Message>& msgs,
                       std::uint32_t context_window,
                       std::uint32_t max_output_tokens,
                       std::size_t keep_recent, const SummaryFn& fn) {
    FoldResult out;
    if (msgs.size() <= keep_recent) return out;

    const std::vector<msg::Message> folded(
        msgs.begin(), msgs.end() - static_cast<std::ptrdiff_t>(keep_recent));
    const std::vector<msg::Message> tail(
        msgs.end() - static_cast<std::ptrdiff_t>(keep_recent), msgs.end());

    const std::string local = local_fold_text(folded);
    std::string text = local;
    std::string reason = "budget forbids an extra LLM call";
    if (fn) {
        const std::string s = fn(folded, local);
        if (!s.empty()) {
            text = s;
            reason = "LLM fold";
        }
    }

    msg::Message summary;
    summary.role = msg::Role::user;
    summary.parts.push_back(
        msg::Text{"[summary of earlier turns]\n" + text});

    out.messages.reserve(tail.size() + 1);
    out.messages.push_back(summary);
    for (const msg::Message& m : tail) out.messages.push_back(m);

    out.summary = std::move(summary);
    out.folded_count = folded.size();
    out.folded_tokens = estimate_history_tokens(folded);
    out.event_what = "history fold: " + std::to_string(folded.size()) +
                     " messages";
    out.event_reason = reason;
    out.folded = true;

    if (needs_fold(out.messages, context_window, max_output_tokens,
                   keep_recent)) {
        /* still over the cap after folding: say so explicitly */
        out.event_reason += "; still over the context cap after fold";
    }
    return out;
}

} /* namespace opencode::memory */
