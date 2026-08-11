#include "bullet_scrape/extractor.hpp"
#include "bullet_scrape/exceptions.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <regex>
#include <set>
#include <shared_mutex>
#include <string_view>
#include <unordered_set>

// Byte-level substring search: SIMD-accelerated memchr on the first byte,
// memcmp to confirm. (memmem is a GNU extension hidden in strict C++17 mode.)
static const char* find_bytes(const char* hay, size_t n,
                              const char* needle, size_t m) noexcept {
    if (m == 0) return hay;
    if (n < m)  return nullptr;
    const char* last = hay + (n - m);          // last position a match can start
    for (;;) {
        const char* p = static_cast<const char*>(
            std::memchr(hay, needle[0], (last - hay) + 1));
        if (!p) return nullptr;
        if (m == 1 || std::memcmp(p + 1, needle + 1, m - 1) == 0) return p;
        hay = p + 1;
    }
}

namespace bullet_scrape {

// ═══════════════════════════════════════════════════════════════════════════
//  Small text helpers
// ═══════════════════════════════════════════════════════════════════════════

static inline bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static std::string trim_str(const std::string& s) {
    size_t start = 0, end = s.size();
    while (start < end && is_ws(s[start])) ++start;
    while (end > start && is_ws(s[end - 1])) --end;
    return s.substr(start, end - start);
}

static inline bool ci_eq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += ('a' - 'A');
        if (y >= 'A' && y <= 'Z') y += ('a' - 'A');
        if (x != y) return false;
    }
    return true;
}

static std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    return out;
}

// Case-insensitive search for a lowercase literal. First match at ≥ `from`.
static size_t find_ci(std::string_view hay, size_t from, const char* needle_lc) {
    const size_t nl = std::strlen(needle_lc);
    const size_t n  = hay.size();
    if (nl == 0) return from <= n ? from : std::string_view::npos;
    if (from + nl > n) return std::string_view::npos;
    const char f0 = needle_lc[0];
    const char f1 = f0 >= 'a' && f0 <= 'z' ? static_cast<char>(f0 - 'a' + 'A') : f0;
    const char* d = hay.data();
    for (size_t i = from; i + nl <= n; ++i) {
        if (d[i] != f0 && d[i] != f1) continue;
        size_t k = 1;
        for (; k < nl; ++k) {
            char c = d[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'a' + 'A');
            if (c != needle_lc[k]) break;
        }
        if (k == nl) return i;
    }
    return std::string_view::npos;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HTML entities
// ═══════════════════════════════════════════════════════════════════════════

// Try to decode the entity starting at s[pos] (which must be '&').
// If recognised, appends the decoded text to `out`, advances `pos` past the
// terminating ';' and returns true; otherwise leaves both untouched and
// returns false.
static bool decode_entity(const std::string_view s, size_t& pos, std::string& out) {
    size_t semi = s.find(';', pos + 1);
    if (semi == std::string_view::npos || semi - pos > 10) return false;
    const std::string_view e = s.substr(pos + 1, semi - pos - 1);

    // Numeric: &#123; or &#x1F;
    if (!e.empty() && e[0] == '#') {
        unsigned long cp = 0;
        size_t i = 1;
        const bool hex = i < e.size() && (e[i] == 'x' || e[i] == 'X');
        if (hex) ++i;
        if (i >= e.size()) return false;
        for (; i < e.size(); ++i) {
            char c = e[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            if (!hex && d > 9) return false;
            cp = cp * (hex ? 16u : 10u) + static_cast<unsigned>(d);
            if (cp > 0x10FFFFu) return false;
        }
        if (cp == 0 || cp > 0x10FFFFu) return false;
        pos = semi + 1;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return true;
    }

    // Named entities — the common set used in real-world markup.
    struct Named { const char* name; const char* val; };
    static const Named kNamed[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
        {"nbsp", " "}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"}, {"deg", "\xC2\xB0"},
        {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"}, {"hellip", "\xE2\x80\xA6"},
        {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"}, {"euro", "\xE2\x82\xAC"},
        {"pound", "\xC2\xA3"}, {"yen", "\xC2\xA5"}, {"cent", "\xC2\xA2"},
        {"trade", "\xE2\x84\xA2"}, {"middot", "\xC2\xB7"}, {"bull", "\xE2\x80\xA2"},
        {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"}, {"ldquo", "\xE2\x80\x9C"},
        {"rdquo", "\xE2\x80\x9D"}, {"times", "\xC3\x97"}, {"divide", "\xC3\xB7"},
        {"plusmn", "\xC2\xB1"}, {"frac12", "\xC2\xBD"}, {"frac14", "\xC2\xBC"},
        {"frac34", "\xC2\xBE"}, {"sect", "\xC2\xA7"}, {"para", "\xC2\xB6"},
        {"micro", "\xC2\xB5"}, {"iexcl", "\xC2\xA1"}, {"iquest", "\xC2\xBF"},
        {"szlig", "\xC3\x9F"}, {"egrave", "\xC3\xA8"}, {"eacute", "\xC3\xA9"},
        {"agrave", "\xC3\xA0"}, {"aacute", "\xC3\xA1"}, {"ccedil", "\xC3\xA7"},
        {"ntilde", "\xC3\xB1"}, {"uuml", "\xC3\xBC"}, {"ouml", "\xC3\xB6"},
        {"auml", "\xC3\xA4"}, {"Uuml", "\xC3\x9C"}, {"Ouml", "\xC3\x96"},
        {"Auml", "\xC3\x84"},
    };
    for (const auto& nd : kNamed) {
        if (e == nd.name) {
            out += nd.val;
            pos = semi + 1;
            return true;
        }
    }
    return false;
}

static bool has_entity(std::string_view s) {
    return s.find('&') != std::string_view::npos;
}

static std::string decode_entities(std::string_view s) {
    if (!has_entity(s)) return std::string(s);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&' && decode_entity(s, i, out)) continue;
        out += s[i];
        ++i;
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Regex helpers (private static members of ExtractionEngine)
// ═══════════════════════════════════════════════════════════════════════════

// Count capturing groups in an ECMAScript pattern: skips escapes, character
// classes, and non-capturing constructs ( (?:, (?=, (?! … ).
static int count_capture_groups(const std::string& pattern) {
    int groups = 0;
    bool escaped = false, in_class = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        char ch = pattern[i];
        if (escaped)   { escaped = false; continue; }
        if (ch == '\\') { escaped = true;  continue; }
        if (in_class)  { if (ch == ']') in_class = false; continue; }
        if (ch == '[') { in_class = true;  continue; }
        if (ch == '(') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '?') continue; // (?: (?= (?! ...
            ++groups;
        }
    }
    return groups;
}

namespace {

// Compiled-pattern cache shared across worker threads. Patterns are stable
// for a job, so the hot path is a shared-lock hit. Compile failures are
// cached too and surfaced as config errors instead of being silently
// replaced with a never-matching pattern.
struct CachedRegex {
    std::regex re;
    bool valid = false;
};

std::unordered_map<std::string, CachedRegex> g_regex_cache;
std::shared_mutex g_regex_cache_mu;

const CachedRegex& get_cached_regex(const std::string& pattern) {
    {
        std::shared_lock lk(g_regex_cache_mu);
        auto it = g_regex_cache.find(pattern);
        if (it != g_regex_cache.end()) return it->second;
    }
    std::unique_lock lk(g_regex_cache_mu);
    auto it = g_regex_cache.find(pattern);
    if (it != g_regex_cache.end()) return it->second;

    CachedRegex entry;
    try {
        entry.re = std::regex(pattern, std::regex::ECMAScript | std::regex::optimize);
        entry.valid = true;
    } catch (const std::regex_error&) {
        entry.valid = false;
    }
    return g_regex_cache.emplace(pattern, std::move(entry)).first->second;
}

// Throws if the pattern cannot be compiled (a misconfiguration, not silence).
const std::regex& compiled_regex(const std::string& pattern, const char* context) {
    const CachedRegex& entry = get_cached_regex(pattern);
    if (!entry.valid)
        throw config_error(std::string("invalid regex in ") + context + ": " + pattern);
    return entry.re;
}

} // anonymous namespace

// ── Match scanning with a literal-prefix anchor ─────────────────────────────

// The longest run of characters the pattern must match *at the start* of
// every match ("" when the pattern has no literal anchor).
static std::string literal_prefix(const std::string& pattern) {
    static const std::string kMeta = "^$.[]{}()*+?|\\";
    std::string out;
    for (char c : pattern) {
        if (kMeta.find(c) != std::string::npos) break;
        out += c;
    }
    // A quantifier applies to the last literal char only: "ab*"/"ab?"/"ab{0}"
    // can match just "a", so that char is not a required part of the prefix.
    if (!out.empty() && out.size() < pattern.size()) {
        const char next = pattern[out.size()];
        if (next == '*' || next == '?' || next == '{')
            out.pop_back();
    }
    return out;
}

// Call `on_match` for every successive non-overlapping match of `re` in
// `text`. When the pattern has a literal prefix, candidate positions are
// found with memchr+memcmp and the engine is only invoked anchored at each
// candidate — libstdc++'s resumable iterator re-scans the whole string with
// its backtracking engine, which this avoids.
using sv_match_iter = std::regex_iterator<std::string_view::const_iterator>;
using sv_match      = std::match_results<std::string_view::const_iterator>;

template <typename F>
static void scan_matches(std::string_view text, const std::string& pattern,
                         const std::regex& re, F&& on_match) {
    const std::string prefix = literal_prefix(pattern);
    if (prefix.size() < 2) {                       // no useful anchor
        for (sv_match_iter it(text.begin(), text.end(), re), end; it != end; ++it)
            on_match(*it);
        return;
    }

    const char* data = text.data();
    const size_t n = text.size();
    size_t pos = 0;
    while (pos + prefix.size() <= n) {
        const char* hit = find_bytes(data + pos, n - pos, prefix.data(), prefix.size());
        if (!hit) break;
        const size_t off = static_cast<size_t>(hit - data);
        sv_match m;
        if (std::regex_search(text.begin() + static_cast<ptrdiff_t>(off), text.end(), m, re,
                              std::regex_constants::match_continuous)) {
            on_match(m);
            const size_t mend = off + static_cast<size_t>(m.length(0));
            pos = mend > off ? mend : off + 1;     // empty matches advance by one
        } else {
            pos = off + 1;
        }
    }
}

std::vector<std::string> ExtractionEngine::regex_find_all(
        std::string_view text, const std::string& pattern) {
    std::vector<std::string> out;
    const auto& re = compiled_regex(pattern, "rule");
    scan_matches(text, pattern, re,
                 [&out](const auto& m) { out.push_back(m.str(0)); });
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HTML scanning primitives (single pass, quote-aware)
// ═══════════════════════════════════════════════════════════════════════════

static inline bool is_tag_name_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline bool is_tag_name_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

// Position of the '>' terminating the tag that starts (with '<') before `pos`,
// honouring single/double quoted attribute values, or npos if unterminated.
static size_t find_tag_end(std::string_view s, size_t pos) {
    char quote = 0;
    for (size_t i = pos; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '>') return i;
    }
    return std::string::npos;
}

// HTML void elements — never have content or a close tag.
static bool is_void_element(const std::string& tag) noexcept {
    static const char* const kVoid[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    for (const char* v : kVoid)
        if (tag == v) return true;
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Document index
// ═══════════════════════════════════════════════════════════════════════════

static bool is_heading_tag(std::string_view t) {
    return t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6';
}

// HTML5 implied-end-tag rules (condensed to the cases that matter for
// scraping): the named opening tag implicitly closes the listed open ones —
// e.g. `<li>` closes a pending `<li>`, any block element closes a `<p>`.
static bool implies_close(std::string_view nw, std::string_view cur) {
    if (nw == "li")          return cur == "li" || cur == "dt" || cur == "dd";
    if (nw == "dt" || nw == "dd") return cur == "dt" || cur == "dd" || cur == "li";
    if (nw == "td" || nw == "th") return cur == "td" || cur == "th";
    if (nw == "tr")          return cur == "tr" || cur == "td" || cur == "th";
    if (nw == "option")      return cur == "option";
    if (nw == "optgroup")    return cur == "option" || cur == "optgroup";
    if (nw == "thead" || nw == "tbody" || nw == "tfoot")
        return cur == "thead" || cur == "tbody" || cur == "tfoot" ||
               cur == "tr" || cur == "td" || cur == "th";
    if (is_heading_tag(nw))  return is_heading_tag(cur);
    return false;
}

// Any block-level opener implicitly ends a pending <p>.
static bool closes_paragraph(std::string_view nw) {
    static const std::unordered_set<std::string_view> kBlocks = {
        "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "dl",
        "table", "section", "article", "aside", "header", "hgroup", "footer",
        "nav", "pre", "blockquote", "form", "fieldset", "address", "hr",
        "main", "figure", "figcaption", "details", "summary"
    };
    return kBlocks.count(nw) != 0;
}

ExtractionEngine::Document ExtractionEngine::parse_document(const std::string& html) {
    // 32-bit offsets: documents up to 4 GiB (originally a static_cast bug
    // truncated offsets to int — better to refuse than silently corrupt).
    if (html.size() > UINT32_MAX)
        throw extract_error("page", "document too large to index (>= 4 GiB)");

    Document doc;
    doc.elements.reserve(512);

    // Tag-name interning without per-event allocation: tag ids index into
    // doc.tag_names (lowercased). New names are rare, so a linear scan over
    // the distinct names (kept as views into the document, compared
    // case-insensitively) beats a hashing std::string copy per tag event.
    std::vector<std::string_view> name_views;
    auto intern = [&](size_t start, size_t len) -> uint32_t {
        const std::string_view v(html.data() + start, len);
        for (uint32_t id = 0; id < name_views.size(); ++id)
            if (name_views[id].size() == len && ci_eq(name_views[id], v))
                return id;
        doc.tag_names.push_back(to_lower(v));
        name_views.push_back(v);
        return static_cast<uint32_t>(name_views.size() - 1);
    };

    // Single open-tag stack (browser-style): a close tag implicitly closes
    // any elements opened inside it.
    struct Open { uint32_t tag, pos, gt; };
    std::vector<Open> stack;
    stack.reserve(64);

    auto emit = [&](uint32_t tag, size_t open, size_t open_gt,
                    size_t close, size_t end) {
        doc.elements.push_back(Element{
            tag, static_cast<uint32_t>(open), static_cast<uint32_t>(open_gt),
            static_cast<uint32_t>(open_gt + 1),
            static_cast<uint32_t>(close), static_cast<uint32_t>(end)});
    };

    const size_t n = html.size();
    size_t i = 0;
    while (i + 1 < n) {
        if (html[i] != '<') { ++i; continue; }
        const char c1 = html[i + 1];

        // ── Markup declarations: comments, doctype, CDATA ─────────────────
        if (c1 == '!') {
            if (html.compare(i + 2, 2, "--") == 0) {            // <!-- comment -->
                const size_t e = html.find("-->", i + 4);
                i = (e == std::string::npos) ? n : e + 3;
                continue;
            }
            if (html.compare(i + 2, 7, "[CDATA[") == 0) {       // <![CDATA[ ... ]]>
                const size_t e = html.find("]]>", i + 9);
                i = (e == std::string::npos) ? n : e + 3;
                continue;
            }
            const size_t gt = html.find('>', i + 2);            // <!DOCTYPE html>
            i = (gt == std::string::npos) ? n : gt + 1;
            continue;
        }
        if (c1 == '?') {                                        // <?php / xml prolog ?>
            const size_t gt = html.find('>', i + 2);
            i = (gt == std::string::npos) ? n : gt + 1;
            continue;
        }

        const bool closing = (c1 == '/');
        const size_t name_start = i + 1 + (closing ? 1 : 0);
        if (name_start >= n || !is_tag_name_start(html[name_start])) { ++i; continue; }

        size_t j = name_start + 1;
        while (j < n && is_tag_name_char(html[j])) ++j;
        const std::string_view name_v(html.data() + name_start, j - name_start);

        // ── <script>/<style> open tags: raw-text content is not HTML ──────
        if (!closing && (ci_eq(name_v, "script") || ci_eq(name_v, "style"))) {
            const size_t gt = find_tag_end(html, j);
            if (gt == std::string::npos) break;
            size_t k = gt;
            while (k > j && is_ws(html[k - 1])) --k;
            const bool self_closing = k > j && html[k - 1] == '/';
            const uint32_t id = intern(name_start, j - name_start);
            if (self_closing) {
                emit(id, i, gt, gt + 1, gt + 1);
                i = gt + 1;
                continue;
            }
            const char* close_pat = ci_eq(name_v, "script") ? "</script" : "</style";
            const size_t cp = find_ci(html, gt + 1, close_pat);
            if (cp == std::string::npos) {
                emit(id, i, gt, n, n);
                break;
            }
            const size_t close_gt = find_tag_end(html, cp + 2 + name_v.size());
            const size_t end = (close_gt == std::string::npos) ? n : close_gt + 1;
            emit(id, i, gt, cp, end);
            i = end;
            continue;
        }

        const uint32_t id = intern(name_start, j - name_start);

        if (closing) {
            // Find the nearest matching open tag; implicitly close anything
            // opened inside it (recovers from missing close tags).
            bool found = false;
            for (size_t d = stack.size(); d-- > 0;) {
                if (stack[d].tag != id) continue;
                found = true;
                const size_t gt = find_tag_end(html, j);
                const size_t close_end = (gt == std::string::npos) ? n : gt + 1;
                for (size_t u = stack.size(); u-- > d + 1;)
                    emit(stack[u].tag, stack[u].pos, stack[u].gt, i, i);
                emit(id, stack[d].pos, stack[d].gt, i, close_end);
                stack.resize(d);
                i = close_end;
                break;
            }
            if (!found) {   // unmatched close tag — skip it
                const size_t gt = html.find('>', j);
                i = (gt == std::string::npos) ? n : gt + 1;
            }
            continue;
        }

        // ── Opening tag ────────────────────────────────────────────────────
        const size_t gt = find_tag_end(html, j);
        if (gt == std::string::npos) break;                     // unterminated tag at EOF
        size_t k = gt;
        while (k > j && is_ws(html[k - 1])) --k;                // allow `<br />`
        const bool self_closing = k > j && html[k - 1] == '/';

        if (self_closing || is_void_element(doc.tag_names[id]))
            emit(id, i, gt, gt + 1, gt + 1);                    // empty content span
        else {
            // Implied end tags: `<li>` auto-closes a pending `<li>`, a block
            // opener closes a pending `<p>`, etc. (browser behaviour).
            const std::string& new_tag = doc.tag_names[id];
            while (!stack.empty()) {
                const std::string& top_tag = doc.tag_names[stack.back().tag];
                if (top_tag == "p" && closes_paragraph(new_tag)) {
                    emit(stack.back().tag, stack.back().pos, stack.back().gt, i, i);
                    stack.pop_back();
                    continue;
                }
                if (implies_close(new_tag, top_tag)) {
                    emit(stack.back().tag, stack.back().pos, stack.back().gt, i, i);
                    stack.pop_back();
                    continue;
                }
                break;
            }
            stack.push_back(Open{id, static_cast<uint32_t>(i),
                                 static_cast<uint32_t>(gt)});
        }
        i = gt + 1;
    }

    // Implicit close at EOF for anything left open — deepest first.
    while (!stack.empty()) {
        emit(stack.back().tag, stack.back().pos, stack.back().gt, n, n);
        stack.pop_back();
    }

    return doc;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Selector parsing & matching (CSS-selector-lite)
// ═══════════════════════════════════════════════════════════════════════════

struct AttrSelector {
    std::string name;
    std::string value;      // empty → presence check only
    bool has_value = false;
};

struct ParsedSelector {
    std::string tag;                    // empty → any of the default tag set
    std::string id;
    std::vector<std::string> classes;
    std::vector<AttrSelector> attrs;
};

// Parse one simple-selector segment (`div#a.b[x="y"]`). Anything that cannot
// belong to a simple selector is skipped, so callers can pass the rightmost
// segment of a compound selector.
static ParsedSelector parse_simple_selector(std::string_view sv) {
    ParsedSelector ps;
    size_t i = 0;
    const size_t n = sv.size();

    auto is_name_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '*' ||
               c == ':' || c == '|';
    };

    if (i < n && (is_name_char(sv[i]) || sv[i] == '*')) {
        const size_t start = i;
        while (i < n && is_name_char(sv[i])) ++i;
        if (sv[start] != '*') ps.tag = to_lower(sv.substr(start, i - start));
    }

    while (i < n) {
        const char c = sv[i];
        if (c == '#') {
            const size_t s = ++i;
            while (i < n && is_name_char(sv[i])) ++i;
            ps.id = std::string(sv.substr(s, i - s));
        } else if (c == '.') {
            const size_t s = ++i;
            while (i < n && is_name_char(sv[i])) ++i;
            if (i > s) ps.classes.emplace_back(sv.substr(s, i - s));
        } else if (c == '[') {
            // Find the matching ']' honouring quoted values.
            size_t e = i + 1;
            char quote = 0;
            for (; e < n; ++e) {
                char q = sv[e];
                if (quote) { if (q == quote) quote = 0; continue; }
                if (q == '"' || q == '\'') { quote = q; continue; }
                if (q == ']') break;
            }
            if (e >= n) break;
            std::string_view body = sv.substr(i + 1, e - i - 1);
            i = e + 1;
            // Trim whitespace
            while (!body.empty() && is_ws(body.front())) body.remove_prefix(1);
            while (!body.empty() && is_ws(body.back()))  body.remove_suffix(1);
            AttrSelector as;
            const size_t eq = body.find('=');
            if (eq == std::string_view::npos) {
                as.name = to_lower(body);
            } else {
                as.name  = to_lower(body.substr(0, eq));
                while (!as.name.empty() && is_ws(as.name.back())) as.name.pop_back();
                std::string_view v = body.substr(eq + 1);
                while (!v.empty() && is_ws(v.front())) v.remove_prefix(1);
                while (!v.empty() && is_ws(v.back()))  v.remove_suffix(1);
                if (v.size() >= 2 &&
                    ((v.front() == '"' && v.back() == '"') ||
                     (v.front() == '\'' && v.back() == '\'')))
                    v = v.substr(1, v.size() - 2);
                as.value = std::string(v);
                as.has_value = true;
            }
            if (!as.name.empty()) ps.attrs.push_back(std::move(as));
        } else {
            ++i;  // whitespace, combinators, pseudo-classes: out of scope
        }
    }
    return ps;
}

static ParsedSelector parse_selector(const std::string& sel) {
    // For compound selectors keep only the rightmost simple segment —
    // e.g. `div.card > a` behaves like `a`, `ul li` behaves like `li`.
    size_t end = sel.size();
    while (end > 0 && (is_ws(sel[end - 1]) || sel[end - 1] == '>' ||
                       sel[end - 1] == '+' || sel[end - 1] == '~'))
        --end;
    size_t begin = end;
    while (begin > 0) {
        const char c = sel[begin - 1];
        if (is_ws(c) || c == '>' || c == '+' || c == '~') break;
        --begin;
    }
    return parse_simple_selector(std::string_view(sel).substr(begin, end - begin));
}

// ── Attribute lookup ────────────────────────────────────────────────────────

// Lex one open tag's attribute list without allocating: `open_tag` must start
// at '<' (any '>' termination works). Returns the raw (undecoded) value view
// of the (case-insensitively) named attribute, an empty view for valueless
// attributes, or nullopt when the attribute is not present.
static std::optional<std::string_view> find_attr_value(std::string_view open_tag,
                                                       std::string_view name_lc) {
    if (open_tag.size() < 2 || open_tag[0] != '<') return std::nullopt;
    size_t i = 1;
    const size_t n = open_tag.size();
    while (i < n && is_tag_name_char(open_tag[i])) ++i;         // skip tag name

    while (i < n) {
        while (i < n && is_ws(open_tag[i])) ++i;
        if (i >= n || open_tag[i] == '>' || open_tag[i] == '/') break;
        const size_t ns = i;
        while (i < n && !is_ws(open_tag[i]) && open_tag[i] != '=' &&
               open_tag[i] != '>' && open_tag[i] != '/')
            ++i;
        const std::string_view name = open_tag.substr(ns, i - ns);
        while (i < n && is_ws(open_tag[i])) ++i;
        std::string_view value;
        if (i < n && open_tag[i] == '=') {
            ++i;
            while (i < n && is_ws(open_tag[i])) ++i;
            if (i < n && (open_tag[i] == '"' || open_tag[i] == '\'')) {
                const char q = open_tag[i++];
                const size_t vs = i;
                while (i < n && open_tag[i] != q) ++i;
                value = open_tag.substr(vs, i - vs);
            } else {
                const size_t vs = i;
                while (i < n && !is_ws(open_tag[i]) && open_tag[i] != '>') ++i;
                value = open_tag.substr(vs, i - vs);
            }
        }
        if (ci_eq(name, name_lc)) return value;
    }
    return std::nullopt;
}

// Tags searched for tag-less selectors (e.g. `.product`).
static bool is_default_candidate(const std::string& tag) {
    static const std::unordered_set<std::string> kDefaults = {
        "div", "span", "a", "p", "li", "tr", "td", "th",
        "section", "article", "main", "table", "ul", "ol"
    };
    return kDefaults.count(tag) != 0;
}

std::vector<ExtractionEngine::Element> ExtractionEngine::find_elements(
        const Document& doc, const std::string& html, const std::string& selector) {
    const ParsedSelector ps = parse_selector(selector);

    auto matches = [&](const Element& el) -> bool {
        const std::string& tag = doc.tag_names[el.tag];
        if (!ps.tag.empty()) {
            if (tag != ps.tag) return false;
        } else if (!is_default_candidate(tag)) {
            return false;
        }

        // Attribute checks only run when needed, against the open tag alone.
        if (ps.id.empty() && ps.classes.empty() && ps.attrs.empty()) return true;

        const std::string_view open_tag(html.data() + el.open, el.open_gt - el.open + 1);

        if (!ps.id.empty()) {
            const auto v = find_attr_value(open_tag, "id");
            if (!v || *v != ps.id) return false;
        }
        if (!ps.classes.empty()) {
            const auto v = find_attr_value(open_tag, "class");
            if (!v) return false;
            for (const auto& cls : ps.classes) {
                bool hit = false;
                size_t i = 0;
                while (i < v->size()) {
                    while (i < v->size() && is_ws((*v)[i])) ++i;
                    const size_t s = i;
                    while (i < v->size() && !is_ws((*v)[i])) ++i;
                    if (i > s && v->substr(s, i - s) == cls) { hit = true; break; }
                }
                if (!hit) return false;
            }
        }
        for (const auto& as : ps.attrs) {
            const auto v = find_attr_value(open_tag, as.name);
            if (!v) return false;
            if (as.has_value && *v != as.value) return false;
        }
        return true;
    };

    std::vector<Element> result;
    for (const auto& el : doc.elements)
        if (matches(el)) result.push_back(el);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Attribute / text extraction
// ═══════════════════════════════════════════════════════════════════════════

std::optional<std::string> ExtractionEngine::extract_attribute(
        std::string_view element_html, std::string_view attr) {
    const std::string attr_lc = to_lower(attr);
    const size_t n = element_html.size();

    // Scan every tag (descendants included); first occurrence wins.
    size_t i = 0;
    while (i + 1 < n) {
        const size_t lt = element_html.find('<', i);
        if (lt == std::string_view::npos || lt + 1 >= n) break;
        const char c1 = element_html[lt + 1];
        if (c1 == '!' || c1 == '?' || c1 == '/' || !is_tag_name_start(c1)) {
            i = lt + 1;
            continue;
        }
        const size_t gt = find_tag_end(element_html, lt + 1);
        if (gt == std::string_view::npos) break;

        if (auto v = find_attr_value(element_html.substr(lt, gt - lt + 1), attr_lc))
            return decode_entities(*v);
        i = gt + 1;
    }
    return std::nullopt;
}

std::string ExtractionEngine::extract_text(std::string_view element_html) {
    std::string out;
    out.reserve(element_html.size());
    bool pending_space = false;   // collapse runs of whitespace to one space
    bool at_word_start = true;    // leading whitespace suppression

    auto push_char = [&](char c) {
        if (is_ws(c)) { pending_space = true; return; }
        if (pending_space && !at_word_start) out += ' ';
        pending_space = false;
        at_word_start = false;
        out += c;
    };

    const size_t n = element_html.size();
    size_t i = 0;
    while (i < n) {
        const char c = element_html[i];
        if (c == '<' && i + 1 < n) {
            const char c1 = element_html[i + 1];
            if (c1 == '!') {                                     // <!-- --> / <!doctype> / CDATA
                if (element_html.compare(i + 2, 2, "--") == 0) {
                    const size_t e = element_html.find("-->", i + 4);
                    i = (e == std::string::npos) ? n : e + 3;
                } else if (element_html.compare(i + 2, 7, "[CDATA[") == 0) {
                    const size_t e = element_html.find("]]>", i + 9);
                    // CDATA *content* is text, unlike comments:
                    const size_t content_end = (e == std::string::npos) ? n : e;
                    for (size_t k = i + 9; k < content_end; ++k) push_char(element_html[k]);
                    i = (e == std::string::npos) ? n : e + 3;
                } else {
                    const size_t e = element_html.find('>', i + 2);
                    i = (e == std::string::npos) ? n : e + 1;
                }
                continue;
            }
            if (c1 == '?') {
                const size_t e = element_html.find('>', i + 2);
                i = (e == std::string::npos) ? n : e + 1;
                continue;
            }
            const bool closing = c1 == '/';
            const size_t name_start = i + 1 + (closing ? 1 : 0);
            if (name_start < n && is_tag_name_start(element_html[name_start])) {
                size_t j = name_start + 1;
                while (j < n && is_tag_name_char(element_html[j])) ++j;
                const size_t gt = find_tag_end(element_html, j);
                if (gt == std::string::npos) break;             // unterminated: drop the rest
                if (!closing) {
                    // <script>/<style>: skip content entirely — it isn't text.
                    const std::string tag =
                        to_lower(std::string_view(element_html.data() + name_start, j - name_start));
                    if (tag == "script" || tag == "style") {
                        size_t k = gt;
                        while (k > j && is_ws(element_html[k - 1])) --k;
                        const bool self_closing = k > j && element_html[k - 1] == '/';
                        if (!self_closing) {
                            const std::string close_pat = "</" + tag;
                            const size_t cp = find_ci(element_html, gt + 1, close_pat.c_str());
                            if (cp == std::string::npos) { i = n; continue; }
                            const size_t close_gt = find_tag_end(element_html, cp + 2 + tag.size());
                            i = (close_gt == std::string::npos) ? n : close_gt + 1;
                            continue;
                        }
                    }
                }
                i = gt + 1;
                continue;
            }
            // Stray '<' (not a tag): treated as literal text below.
        }
        if (c == '&') {
            std::string decoded;
            if (decode_entity(element_html, i, decoded)) {
                for (char d : decoded) push_char(d);   // nbsp still collapses
                continue;
            }
        }
        push_char(element_html[i]);
        ++i;
    }
    // `pending_space` at end is simply dropped → trailing trim for free.
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  URL join helper
// ═══════════════════════════════════════════════════════════════════════════

static std::string urljoin(const std::string& base, const std::string& rel) {
    if (rel.empty())  return base;
    if (base.empty()) return rel;
    if (rel.find("://") != std::string::npos) return rel;      // absolute

    const size_t se = base.find("://");
    if (se == std::string::npos) return rel;                   // base not absolute
    const std::string_view scheme(base.data(), se);
    const size_t ps = base.find('/', se + 3);                  // path start
    const std::string host = base.substr(
        se + 3, ps == std::string::npos ? std::string::npos : ps - se - 3);

    if (rel.compare(0, 2, "//") == 0) {                        // protocol-relative
        std::string out;
        out.reserve(se + 1 + rel.size());
        out.append(scheme).append(":").append(rel);
        return out;
    }
    if (rel[0] == '/') {                                       // root-relative
        std::string out;
        out.reserve(se + 3 + host.size() + rel.size());
        out.append(scheme).append("://").append(host).append(rel);
        return out;
    }

    // Document-relative: resolve against the base directory.
    const std::string_view base_path =
        ps == std::string::npos ? std::string_view("/") : std::string_view(base).substr(ps);
    const size_t ls = base_path.rfind('/');
    const std::string_view parent = base_path.substr(0, ls + 1);

    std::string out;
    out.reserve(se + 3 + host.size() + parent.size() + rel.size());
    out.append(scheme).append("://").append(host).append(parent);
    for (char c : rel) {
        if (c == ' ') out += "%20";                            // minimal escaping
        else          out += c;
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Transform
// ═══════════════════════════════════════════════════════════════════════════

json ExtractionEngine::apply_transforms(
        const std::vector<std::string>& values,
        const ExtractRule& rule,
        const std::string& base_url) {
    json out = json::array();

    // A trailing int/float conversion determines the JSON value type.
    const bool to_int   = !rule.transform.empty() && rule.transform.back() == "int";
    const bool to_float = !rule.transform.empty() && rule.transform.back() == "float";

    for (const auto& raw : values) {
        std::string v = raw;
        bool ok = true;
        for (const auto& t : rule.transform) {
            if      (t == "trim")      v = trim_str(v);
            else if (t == "lowercase") { for (auto& c : v) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); }
            else if (t == "uppercase") { for (auto& c : v) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); }
            else if (t == "urljoin")   v = urljoin(base_url, v);
            else if (t == "int") {
                try { v = std::to_string(std::stoll(v)); }
                catch (...) { ok = false; break; }
            }
            else if (t == "float") {
                try { v = std::to_string(std::stod(v)); }
                catch (...) { ok = false; break; }
            }
            else if (t == "regex_sub" && rule.regex_sub) {
                const auto& re = compiled_regex(rule.regex.value_or(""), "regex_sub");
                v = std::regex_replace(v, re, *rule.regex_sub);
            }
        }

        if (!ok) {
            out.push_back(nullptr);            // failed int/float conversion → null
        } else if (to_int) {
            try   { out.push_back(std::stoll(v)); }
            catch (...) { out.push_back(nullptr); }
        } else if (to_float) {
            try   { out.push_back(std::stod(v)); }
            catch (...) { out.push_back(nullptr); }
        } else {
            out.push_back(std::move(v));
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Aggregate (operates on the json array output of apply_transforms)
// ═══════════════════════════════════════════════════════════════════════════

json ExtractionEngine::apply_aggregate(const json& transformed,
                                       const ExtractRule& rule,
                                       size_t raw_count) {
    if (!rule.aggregate) return transformed;

    const auto& agg = *rule.aggregate;

    if (agg == "count")  return static_cast<long long>(raw_count);
    if (agg == "exists") return raw_count > 0;

    if (agg == "first") {
        if (transformed.size() == 0) return json(nullptr);
        return transformed[0];
    }
    if (agg == "last") {
        if (transformed.size() == 0) return json(nullptr);
        return transformed[transformed.size() - 1];
    }

    if (agg == "unique") {
        std::set<std::string> seen;
        json result = json::array();
        for (size_t i = 0; i < transformed.size(); ++i) {
            const std::string val = transformed[i].is_string()
                ? transformed[i].get_string()
                : transformed[i].dump();
            if (seen.insert(val).second)
                result.push_back(transformed[i]);
        }
        return result;
    }

    if (agg == "join") {
        const std::string sep = rule.join_sep.value_or("");
        std::string joined;
        for (size_t i = 0; i < transformed.size(); ++i) {
            if (i) joined += sep;
            joined += transformed[i].is_string()
                ? transformed[i].get_string()
                : transformed[i].dump();
        }
        return joined;
    }

    return transformed;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Leaf extraction
// ═══════════════════════════════════════════════════════════════════════════

json ExtractionEngine::extract_leaf(const SubQuery& sub,
                                    std::string_view element_html,
                                    const std::string& base_url) {
    const auto& rule = sub.rule;
    std::vector<std::string> raw;

    if (rule.text) {
        raw = {extract_text(element_html)};
    } else if (rule.attribute) {
        if (auto val = extract_attribute(element_html, *rule.attribute))
            raw = {std::move(*val)};
    } else if (rule.regex) {
        // Pull the first capture group when present, else the whole match.
        const auto& re = compiled_regex(*rule.regex, "rule");
        const size_t pick = count_capture_groups(*rule.regex) >= 1 ? 1 : 0;
        scan_matches(element_html, *rule.regex, re, [&](const sv_match& m) {
            raw.push_back(m.size() > pick ? m.str(pick) : m.str(0));
        });
    }

    if (raw.empty()) {
        if (rule.optional)  return json(nullptr);
        if (rule.aggregate) return apply_aggregate(json::array(), rule, 0);
        return json::array();
    }

    json transformed = apply_transforms(raw, rule, base_url);
    return apply_aggregate(transformed, rule, raw.size());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Full query execution
// ═══════════════════════════════════════════════════════════════════════════

std::vector<Record> ExtractionEngine::execute(
        const ScraperConfig& cfg,
        const std::string& html,
        const std::string& base_url) {

    std::vector<Record> all_records;
    std::optional<Document> doc;                 // built on first selector query

    // Cross-element aggregation helper: pure collection-level aggregates
    // (count/exists with no extraction fields) are answered directly from the
    // element list, and `exists` merges as "any element yielded a value"
    // rather than re-running a generic aggregate over per-element booleans.
    auto merge_into_record =
        [this](Record& merged, const SubQuery& sub,
               const std::vector<Record>& per_element, size_t element_count) {
        const auto& rule = sub.rule;
        const std::string agg = rule.aggregate.value_or("");
        const bool no_extraction = !rule.text && !rule.attribute && !rule.regex;

        if (agg == "count" && no_extraction && rule.transform.empty()) {
            merged[sub.name] = static_cast<long long>(element_count);
            return;
        }
        if (agg == "exists" && no_extraction && rule.transform.empty()) {
            merged[sub.name] = element_count > 0;
            return;
        }

        // Collect all values for this field across all elements.
        json all_values = json::array();
        for (const auto& rec : per_element) {
            auto it = rec.find(sub.name);
            if (it == rec.end()) continue;
            const auto& val = it->second;
            if (val.is_array()) {
                for (size_t i = 0; i < val.size(); ++i)
                    all_values.push_back(val[i]);
            } else {
                all_values.push_back(val);
            }
        }
        if (agg == "exists") {                   // any element yielded a value
            bool any = false;
            for (size_t i = 0; i < all_values.size(); ++i) {
                const json& v = all_values[i];
                any |= v.is_null() ? false : (v.is_bool() ? v.get_bool() : true);
            }
            merged[sub.name] = any;
            return;
        }
        merged[sub.name] = apply_aggregate(all_values, rule, all_values.size());
    };

    for (const auto& [qname, query] : cfg.queries) {
        if (query.xpath) continue;               // XPath requires a full DOM — unsupported here

        // ── Raw-regex query ────────────────────────────────────────────────
        if (query.regex) {
            const auto& re = compiled_regex(*query.regex, "query");
            const size_t pick = count_capture_groups(*query.regex) >= 1 ? 1 : 0;
            scan_matches(html, *query.regex, re, [&](const sv_match& m) {
                const std::string element_text =
                    m.size() > pick ? m.str(pick) : m.str(0);

                Record rec;
                for (const auto& sub : query.extract) {
                    if (sub.rule.regex) {
                        auto matches = regex_find_all(element_text, *sub.rule.regex);
                        if (matches.empty()) {
                            rec[sub.name] = sub.rule.optional ? json(nullptr) : json::array();
                        } else {
                            json t = apply_transforms(matches, sub.rule, base_url);
                            rec[sub.name] = apply_aggregate(t, sub.rule, matches.size());
                        }
                    } else {
                        json t = apply_transforms({element_text}, sub.rule, base_url);
                        rec[sub.name] = apply_aggregate(t, sub.rule, 1);
                    }
                }
                all_records.push_back(std::move(rec));
            });
            continue;
        }

        // ── Selector-based query ───────────────────────────────────────────
        if (!doc) doc = parse_document(html);
        auto elements = find_elements(*doc, html, query.selector);

        const bool has_aggregate =
            std::any_of(query.extract.begin(), query.extract.end(),
                        [](const SubQuery& s) { return s.rule.aggregate.has_value(); });

        const std::string_view html_v(html);

        if (has_aggregate && query.multiple) {
            // Cross-element aggregate: per-element extraction, then merge.
            std::vector<Record> per_element;
            per_element.reserve(elements.size());
            for (const auto& el : elements) {
                Record rec;
                for (const auto& sub : query.extract)
                    rec[sub.name] = extract_leaf(
                        sub, html_v.substr(el.open, el.end - el.open), base_url);
                per_element.push_back(std::move(rec));
            }

            Record merged;
            for (const auto& sub : query.extract)
                merge_into_record(merged, sub, per_element, elements.size());
            all_records.push_back(std::move(merged));
        } else {
            if (!query.multiple && elements.size() > 1)
                elements.resize(1);
            for (const auto& el : elements) {
                Record rec;
                for (const auto& sub : query.extract)
                    rec[sub.name] = extract_leaf(
                        sub, html_v.substr(el.open, el.end - el.open), base_url);
                all_records.push_back(std::move(rec));
            }
        }
    }

    return all_records;
}

json ExtractionEngine::execute_scalar(const CollectionQuery& query,
                                      const std::string& html) {
    if (query.extract.empty()) return json(nullptr);
    json rec = json::object();
    for (const auto& sub : query.extract)
        rec[sub.name] = extract_leaf(sub, html, "");
    return rec;
}

} // namespace bullet_scrape
