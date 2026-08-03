/*
 * compiler.cpp -- template text -> PromptRef.
 *
 * The single substitution mechanism is `{{PLACEHOLDER}}`; everything else is
 * literal text. The compiler never does runtime templating: it discovers the
 * slot names, hashes the compiled form (sha1) and estimates its tokens, so
 * prompt drift is visible to tests and the binary pins its own prompt hash.
 * Never throws.
 */
#include "prompt/compiler.h"

#include <cstdio>
#include <string>
#include <utility>

#include "util/sha1.h"

namespace opencode::prompt {

bool PromptRef::has_placeholder(std::string_view name) const noexcept {
    for (const PromptPart& p : parts)
        for (const std::string& ph : p.placeholders)
            if (ph == name) return true;
    return false;
}

namespace {

/* Discover `{{NAME}}` slots in `text`, in order, deduped. */
void find_placeholders(std::string_view text, std::vector<std::string>& out) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t open = text.find("{{", pos);
        if (open == std::string_view::npos) break;
        const size_t close = text.find("}}", open + 2);
        if (close == std::string_view::npos) break;
        const std::string name(text.substr(open + 2, close - open - 2));
        bool dup = false;
        for (const std::string& have : out)
            if (have == name) {
                dup = true;
                break;
            }
        if (!dup && !name.empty()) out.push_back(name);
        pos = close + 2;
    }
}

} /* namespace */

core::error_code compile_prompt(std::string_view id, std::string_view source,
                                PromptRef& out) {
    PromptRef r;
    r.id = std::string(id);
    r.sha1 = util::sha1_hex(std::string(id) + "\n" + std::string(source));

    PromptPart part;
    part.name = std::string(id);
    part.text = std::string(source);
    find_placeholders(source, part.placeholders);
    part.kind = part.placeholders.empty() ? PromptPartKind::system
                                          : PromptPartKind::template_;
    r.parts.push_back(std::move(part));

    r.estimated_tokens = static_cast<std::uint32_t>(
        msg::estimate_tokens(source));
    out = std::move(r);
    return core::ok();
}

core::error_code compile_prompt_file(const std::string& id,
                                     const std::string& path,
                                     PromptRef& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return core::make_error_code(core::Err::e_missing_cfg);
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    std::fclose(f);
    return compile_prompt(id, text, out);
}

} /* namespace opencode::prompt */
