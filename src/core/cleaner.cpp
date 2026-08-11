#include "bullet_scrape/cleaner.hpp"

#include <cctype>

namespace bullet_scrape {

// ── Small byte/UTF-8 helpers ────────────────────────────────────────────────
// Scraped pages are overwhelmingly ASCII, so every stage is written as a byte
// loop with targeted multi-byte sequence checks — no full UTF-8 decoding.

static inline bool is_ws_byte(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Decode the UTF-8 sequence starting at s[i] (which is a lead byte >= 0x80).
// Writes the codepoint to `cp` and returns its byte length, or 0 when invalid.
static inline int utf8_decode(std::string_view s, size_t i, uint32_t& cp) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) { cp = b0; return 1; }
    const size_t n = s.size();
    if (b0 >= 0xC2 && b0 <= 0xDF) {                    // 2-byte
        if (i + 1 >= n) return 0;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        if ((b1 & 0xC0) != 0x80) return 0;
        cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
        return 2;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {                    // 3-byte
        if (i + 2 >= n) return 0;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        const auto b2 = static_cast<unsigned char>(s[i + 2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
        cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        if (cp < 0x800) return 0;
        return 3;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {                    // 4-byte
        if (i + 3 >= n) return 0;
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        const auto b2 = static_cast<unsigned char>(s[i + 2]);
        const auto b3 = static_cast<unsigned char>(s[i + 3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80)
            return 0;
        cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) |
             ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return 0;
        return 4;
    }
    return 0;                                          // stray continuation / bad lead
}

static inline bool is_unicode_space(uint32_t cp) noexcept {
    return cp == 0xA0 ||                                // no-break space
           (cp >= 0x2000 && cp <= 0x200A) ||            // en/em quads, thin sp…
           cp == 0x2028 || cp == 0x2029 ||              // line/paragraph separators
           cp == 0x202F || cp == 0x205F ||
           cp == 0x1680 || cp == 0x3000 || cp == 0x85;  // ogham, ideographic, NEL
}

static inline bool is_invisible(uint32_t cp) noexcept {
    return cp == 0xAD ||                                // soft hyphen
           (cp >= 0x200B && cp <= 0x200F) ||            // ZWSP ZWNJ ZWJ LRM RLM
           (cp >= 0x202A && cp <= 0x202E) ||            // bidi embeddings/overrides
           (cp >= 0x2060 && cp <= 0x2064) ||            // word joiner & friends
           cp == 0xFEFF;                                // BOM / zero-width NBSP
}

// ═══════════════════════════════════════════════════════════════════════════
//  Stage: entities
// ═══════════════════════════════════════════════════════════════════════════

bool html_entity_decode(std::string_view s, size_t& pos, std::string& out) {
    size_t semi = s.find(';', pos + 1);
    if (semi == std::string_view::npos || semi - pos > 10) return false;
    const std::string_view e = s.substr(pos + 1, semi - pos - 1);

    // Numeric: &#123; / &#x1F;
    if (!e.empty() && e[0] == '#') {
        uint32_t cp = 0;
        size_t i = 1;
        const bool hex = i < e.size() && (e[i] == 'x' || e[i] == 'X');
        if (hex) ++i;
        if (i >= e.size()) return false;
        for (; i < e.size(); ++i) {
            const char c = e[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            cp = cp * (hex ? 16u : 10u) + static_cast<uint32_t>(d);
            if (cp > 0x10FFFFu) return false;
        }
        if (cp == 0) return false;
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

    // Named — the set seen on real pages.
    struct Named { const char* name; const char* val; };
    static const Named kNamed[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
        {"nbsp", "\xC2\xA0"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"},
        {"deg", "\xC2\xB0"}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},
        {"hellip", "\xE2\x80\xA6"}, {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"},
        {"euro", "\xE2\x82\xAC"}, {"pound", "\xC2\xA3"}, {"yen", "\xC2\xA5"},
        {"cent", "\xC2\xA2"}, {"trade", "\xE2\x84\xA2"}, {"middot", "\xC2\xB7"},
        {"bull", "\xE2\x80\xA2"}, {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
        {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}, {"times", "\xC3\x97"},
        {"divide", "\xC3\xB7"}, {"plusmn", "\xC2\xB1"}, {"frac12", "\xC2\xBD"},
        {"frac14", "\xC2\xBC"}, {"frac34", "\xC2\xBE"}, {"sect", "\xC2\xA7"},
        {"para", "\xC2\xB6"}, {"micro", "\xC2\xB5"}, {"iexcl", "\xC2\xA1"},
        {"iquest", "\xC2\xBF"}, {"szlig", "\xC3\x9F"}, {"egrave", "\xC3\xA8"},
        {"eacute", "\xC3\xA9"}, {"agrave", "\xC3\xA0"}, {"aacute", "\xC3\xA1"},
        {"ccedil", "\xC3\xA7"}, {"ntilde", "\xC3\xB1"}, {"uuml", "\xC3\xBC"},
        {"ouml", "\xC3\xB6"}, {"auml", "\xC3\xA4"}, {"Uuml", "\xC3\x9C"},
        {"Ouml", "\xC3\x96"}, {"Auml", "\xC3\x84"},
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

// ═══════════════════════════════════════════════════════════════════════════
//  Individual stage passes (used for non-default masks)
// ═══════════════════════════════════════════════════════════════════════════

static void stage_entities(std::string& v) {
    if (v.find('&') == std::string::npos) return;
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size();) {
        if (v[i] == '&' && html_entity_decode(v, i, out)) continue;
        out += v[i];
        ++i;
    }
    v = std::move(out);
}

static void stage_invisibles(std::string& v) {
    std::string out;
    out.reserve(v.size());
    const size_t n = v.size();
    for (size_t i = 0; i < n;) {
        const unsigned char b = static_cast<unsigned char>(v[i]);
        if (b < 0x80) {
            // C0 controls (keep \t \n \r), DEL.
            if ((b < 0x20 && b != '\t' && b != '\n' && b != '\r') || b == 0x7F) { ++i; continue; }
            out += v[i++];
            continue;
        }
        uint32_t cp = 0;
        const int len = utf8_decode(v, i, cp);
        if (len == 0) { ++i; continue; }               // drop broken byte
        if ((cp >= 0x80 && cp <= 0x9F) || is_invisible(cp)) { i += len; continue; }
        out.append(v, i, len);
        i += len;
    }
    v = std::move(out);
}

static void stage_whitespace(std::string& v) {
    std::string out;
    out.reserve(v.size());
    bool pending = false, at_start = true;
    const size_t n = v.size();
    for (size_t i = 0; i < n;) {
        const unsigned char b = static_cast<unsigned char>(v[i]);
        if (b < 0x80) {
            if (is_ws_byte(static_cast<char>(b))) { pending = true; ++i; continue; }
            if (pending && !at_start) out += ' ';
            pending = at_start = false;
            out += v[i++];
            continue;
        }
        uint32_t cp = 0;
        const int len = utf8_decode(v, i, cp);
        if (len == 0) { ++i; continue; }
        if (is_unicode_space(cp)) { pending = true; i += len; continue; }
        if (pending && !at_start) out += ' ';
        pending = at_start = false;
        out.append(v, i, len);
        i += len;
    }
    v = std::move(out);
}

static void stage_fold(std::string& v) {
    std::string out;
    out.reserve(v.size());
    const size_t n = v.size();
    for (size_t i = 0; i < n;) {
        const unsigned char b = static_cast<unsigned char>(v[i]);
        if (b < 0x80) { out += v[i++]; continue; }
        uint32_t cp = 0;
        const int len = utf8_decode(v, i, cp);
        if (len == 0) { ++i; continue; }
        const char* to = nullptr;
        switch (cp) {
            case 0x2018: case 0x2019: case 0x2032: to = "'";   break;
            case 0x201C: case 0x201D: case 0x2033: to = "\"";  break;
            case 0x2013: case 0x2014: case 0x2012: case 0x2015: to = "-"; break;
            case 0x2026: to = "..."; break;
            case 0xAB: case 0xBB: to = "\""; break;
            case 0x2039: case 0x203A: to = "'"; break;
            default: break;
        }
        if (to) { out += to; i += len; continue; }
        out.append(v, i, len);
        i += len;
    }
    v = std::move(out);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Fused default pass: entities + invisibles + whitespace in ONE sweep
// ═══════════════════════════════════════════════════════════════════════════

static void stage_fused_default(std::string& v) {
    std::string out;
    out.reserve(v.size());
    bool pending = false, at_start = true;

    auto emit = [&](char c) {                    // single, already clean byte
        if (is_ws_byte(c)) { pending = true; return; }
        if (pending && !at_start) out += ' ';
        pending = at_start = false;
        out += c;
    };
    // Emit the multi-byte sequence starting at `from`: whitespace → collapse,
    // invisible → drop, otherwise keep verbatim. Returns its byte length
    // (1 for a broken byte, which is dropped).
    auto emit_bytes = [&](std::string_view bytes, size_t from) -> size_t {
        uint32_t cp = 0;
        const int l = utf8_decode(bytes, from, cp);
        if (l == 0) return 1;
        if ((cp >= 0x80 && cp <= 0x9F) || is_invisible(cp)) return static_cast<size_t>(l);
        if (is_unicode_space(cp)) { pending = true; return static_cast<size_t>(l); }
        if (pending && !at_start) out += ' ';
        pending = at_start = false;
        out.append(bytes, from, static_cast<size_t>(l));
        return static_cast<size_t>(l);
    };

    const size_t n = v.size();
    size_t i = 0;
    while (i < n) {
        const char c = v[i];
        if (c == '&') {
            std::string decoded;
            if (html_entity_decode(v, i, decoded)) {
                // re-sieve decoded bytes (e.g. &nbsp; is a space)
                for (size_t k = 0; k < decoded.size();) {
                    const unsigned char d = static_cast<unsigned char>(decoded[k]);
                    if (d < 0x80) { emit(decoded[k]); ++k; }
                    else k += emit_bytes(decoded, k);
                }
                continue;
            }
            emit(c); ++i; continue;
        }
        const unsigned char b = static_cast<unsigned char>(c);
        if (b < 0x80) {
            if ((b < 0x20 && b != '\t' && b != '\n' && b != '\r') || b == 0x7F)
                { ++i; continue; }               // invisible: drop
            emit(c); ++i; continue;
        }
        i += emit_bytes(v, i);
    }
    v = std::move(out);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Numeric canonicalisation (CLEAN_NUMERIC stage + int/float transforms)
// ═══════════════════════════════════════════════════════════════════════════

static inline bool is_currency_byte_seq(std::string_view s, size_t i, size_t& len) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if (b == '$') { len = 1; return true; }
    if (b == 0xC2 && i + 1 < s.size()) {          // £ ¥ ¢
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        if (b1 == 0xA3 || b1 == 0xA5 || b1 == 0xA2) { len = 2; return true; }
    }
    if (b == 0xE2 && i + 2 < s.size()) {          // € ₹ ₽ ₩
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        if (b1 == 0x82 && (b2 == 0xAC || b2 == 0xB9 || b2 == 0xBD || b2 == 0xA9))
            { len = 3; return true; }
    }
    if (b == 0xE0 && i + 2 < s.size()) {          // ฿
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        if (b1 == 0xB8 && b2 == 0xBF) { len = 3; return true; }
    }
    return false;
}

bool clean_numeric_inplace(std::string& v) {
    std::string_view s(v);
    size_t i = 0;
    const size_t n = s.size();

    // leading clutter ↔ optional sign ↔ more clutter — accept "$1,299",
    // "-€2,000", "€ -2,000" alike.
    bool neg = false;
    for (int pass = 0; pass < 2; ++pass) {
        while (i < n) {
            if (is_ws_byte(s[i])) { ++i; continue; }
            size_t cl = 0;
            if (is_currency_byte_seq(s, i, cl)) { i += cl; continue; }
            if (pass == 0 && (s[i] == '-' || s[i] == '+') && i + 1 < n) {
                neg = s[i] == '-';
                ++i;
                continue;                         // allow "$ - 50" orderings
            }
            break;
        }
    }

    // integer part: digits with optional ',' thousands groups — capture the
    // raw span first, then validate the grouping strictly.
    const size_t int_start = i;
    while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == ',')) ++i;
    const std::string_view int_span = s.substr(int_start, i - int_start);
    if (int_span.empty()) return false;

    std::string digits;
    digits.reserve(int_span.size());
    if (int_span.find(',') == std::string_view::npos) {
        digits = std::string(int_span);
    } else {
        // Comma form must be exactly d{1,3}(,d{3})+ — "12,34" is not numeric.
        const size_t first = int_span.find(',');
        if (first == 0 || first > 3) return false;        // 1–3 digits before ','
        for (size_t k = 0; k < first; ++k)
            if (int_span[k] < '0' || int_span[k] > '9') return false;
        for (size_t k = first; k < int_span.size(); k += 4) {
            if (int_span[k] != ',') return false;         // then exactly ",ddd"
            if (k + 4 > int_span.size()) return false;
            for (size_t g = 1; g <= 3; ++g) {
                const char c = int_span[k + g];
                if (c < '0' || c > '9') return false;
            }
        }
        for (char c : int_span)
            if (c != ',') digits += c;
    }

    std::string frac;
    if (i < n && s[i] == '.') {
        ++i;
        for (; i < n; ++i) {
            const char c = s[i];
            if (c >= '0' && c <= '9') { frac += c; continue; }
            break;
        }
    }

    // allowed trailing debris: whitespace, currency, a single '%'
    bool trailing = false;
    while (i < n) {
        const char c = s[i];
        if (is_ws_byte(c)) { ++i; continue; }
        if (c == '%' && !trailing) { trailing = true; ++i; continue; }
        size_t cl = 0;
        if (is_currency_byte_seq(s, i, cl)) { i += cl; continue; }
        return false;                                     // letters etc → not numeric
    }

    if (digits.empty() && frac.empty()) return false;

    std::string out;
    out.reserve(1 + digits.size() + 1 + frac.size());
    if (neg) out += '-';
    out += digits.empty() ? "0" : digits;
    if (!frac.empty()) { out += '.'; out += frac; }
    v = std::move(out);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Null tokens
// ═══════════════════════════════════════════════════════════════════════════

bool is_null_token(std::string_view v) {
    if (v.empty()) return true;
    if (v.size() > 7) return false;
    char buf[8];
    for (size_t i = 0; i < v.size(); ++i) {
        char c = v[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view lc(buf, v.size());
    static constexpr std::string_view kTokens[] = {
        "", "null", "none", "n/a", "na", "nil", "unknown", "-", "--"
    };
    for (auto t : kTokens)
        if (lc == t) return true;
    return v == "\xE2\x80\x94";                           // em dash
}

// ═══════════════════════════════════════════════════════════════════════════
//  Sieve driver
// ═══════════════════════════════════════════════════════════════════════════

std::optional<uint32_t> clean_stage_by_name(std::string_view name) {
    struct Entry { const char* name; uint32_t bit; };
    static constexpr Entry kStages[] = {
        {"entities",    CLEAN_ENTITIES},
        {"invisibles",  CLEAN_INVISIBLES},
        {"whitespace",  CLEAN_WHITESPACE},
        {"ws",          CLEAN_WHITESPACE},
        {"fold",        CLEAN_FOLD},
        {"numeric",     CLEAN_NUMERIC},
        {"null_tokens", CLEAN_NULL_TOKEN},
        {"nulls",       CLEAN_NULL_TOKEN},
    };
    for (const auto& e : kStages)
        if (name == e.name) return e.bit;
    return std::nullopt;
}

void apply_cleaners(std::string& value, uint32_t mask) {
    if (mask == 0 || value.empty()) return;
    const uint32_t fused =
        mask & (CLEAN_ENTITIES | CLEAN_INVISIBLES | CLEAN_WHITESPACE);
    if (fused == (CLEAN_ENTITIES | CLEAN_INVISIBLES | CLEAN_WHITESPACE)) {
        if (value.find('&') == std::string::npos &&
            value.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "0123456789 \t\n\r\f\v!@#%^*()-_=+[]{}|;:,.<>?/'\"\\`~")
                == std::string::npos)
            stage_whitespace(value);       // pure-ASCII fast path
        else
            stage_fused_default(value);
    } else {
        if (fused & CLEAN_ENTITIES)   stage_entities(value);
        if (fused & CLEAN_INVISIBLES) stage_invisibles(value);
        if (fused & CLEAN_WHITESPACE) stage_whitespace(value);
    }
    if (mask & CLEAN_FOLD)    stage_fold(value);
    if (mask & CLEAN_NUMERIC) clean_numeric_inplace(value);
    // CLEAN_NULL_TOKEN is applied at JSON-emission time (value → null).
}

} // namespace bullet_scrape
