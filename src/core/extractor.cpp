// ============================================================================
//  extractor.cpp — extraction engine
//
//  Pipeline per page:
//      raw HTML bytes ──► Document index (one pass, no DOM)
//                    ──► find_elements: selector → element spans
//                    ──► extract_leaf: text / attribute / regex per element
//                    ──► transforms ──► aggregates ──► Records
//
//  Performance notes:
//    • No DOM is ever materialised for selector queries; elements are POD
//      byte-span structs pointing into the page.
//    • Regex rules with a recognised simple shape (literal + one bounded
//      capture) are executed with memchr/memcmp — the "fast pattern" path —
//      instead of the backtracking std::regex engine, which dominates any
//      profile of regex-heavy scraping. Full ECMAScript patterns transparently
//      fall back to a cached std::regex, seeded by their literal prefix.
//    • Text extraction has a zero-alloc fast path for already-clean text.
// ============================================================================
#include "bullet_scrape/extractor.hpp"
#include "bullet_scrape/exceptions.hpp"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <mutex>
#include <regex>
#include <set>
#include <shared_mutex>
#include <string_view>
#include <unordered_set>
#include <utility>

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

static std::string trim_str(std::string_view s) {
    size_t start = 0, end = s.size();
    while (start < end && is_ws(s[start])) ++start;
    while (end > start && is_ws(s[end - 1])) --end;
    return std::string(s.substr(start, end - start));
}

static inline char lc(char c) noexcept {          // ASCII lower
    return (c >= 'A' && c <= 'Z')
        ? static_cast<char>(c + ('a' - 'A')) : c;
}

static inline bool ci_eq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lc(a[i]) != lc(b[i])) return false;
    return true;
}

static std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = lc(c);
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
        for (; k < nl; ++k)
            if (lc(d[i + k]) != needle_lc[k]) break;
        if (k == nl) return i;
    }
    return std::string_view::npos;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HTML entities
// ═══════════════════════════════════════════════════════════════════════════

// Append `cp` to `out` as UTF-8.
static void append_utf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
}

// Named references covering what actually shows up in scraped pages
// (core Latin-1 + punctuation + symbols), sorted for binary search.
struct NamedEntity { const char* name; const char* utf8; };
static const NamedEntity kEntities[] = {
    {"AElig",  "\xC3\x86"}, {"Aacute", "\xC3\x81"}, {"Acirc",  "\xC3\x82"},
    {"Agrave", "\xC3\x80"}, {"Alpha",  "\xCE\x91"}, {"Aring",  "\xC3\x85"},
    {"Atilde", "\xC3\x83"}, {"Auml",   "\xC3\x84"}, {"Beta",   "\xCE\x92"},
    {"Ccedil", "\xC3\x87"}, {"Chi",    "\xCE\xA7"}, {"Dagger", "\xE2\x80\xA1"},
    {"Delta",  "\xCE\x94"}, {"ETH",    "\xC3\x90"}, {"Eacute", "\xC3\x89"},
    {"Ecirc",  "\xC3\x8A"}, {"Egrave", "\xC3\x88"}, {"Epsilon","\xCE\x95"},
    {"Eta",    "\xCE\x97"}, {"Euml",   "\xC3\x8B"}, {"Gamma",  "\xCE\x93"},
    {"Iacute", "\xC3\x8D"}, {"Icirc",  "\xC3\x8E"}, {"Igrave", "\xC3\x8C"},
    {"Iota",   "\xCE\x99"}, {"Iuml",   "\xC3\x8F"}, {"Kappa",  "\xCE\x9A"},
    {"Lambda", "\xCE\x9B"}, {"Mu",     "\xCE\x9C"}, {"Ntilde", "\xC3\x91"},
    {"Nu",     "\xCE\x9D"}, {"OElig",  "\xC5\x92"}, {"Oacute", "\xC3\x93"},
    {"Ocirc",  "\xC3\x94"}, {"Ograve", "\xC3\x92"}, {"Omega",  "\xCE\xA9"},
    {"Omicron","\xCE\x9F"}, {"Oslash", "\xC3\x98"}, {"Otilde", "\xC3\x95"},
    {"Ouml",   "\xC3\x96"}, {"Phi",    "\xCE\xA6"}, {"Pi",     "\xCE\xA0"},
    {"Prime",  "\xE2\x80\xB3"}, {"Psi","\xCE\xA8"}, {"Rho",    "\xCE\xA1"},
    {"Scaron", "\xC5\xA0"}, {"Sigma",  "\xCE\xA3"}, {"THORN",  "\xC3\x9E"},
    {"Tau",    "\xCE\xA4"}, {"Theta",  "\xCE\x98"}, {"Uacute", "\xC3\x9A"},
    {"Ucirc",  "\xC3\x9B"}, {"Ugrave", "\xC3\x99"}, {"Upsilon","\xCE\xA5"},
    {"Uuml",   "\xC3\x9C"}, {"Xi",     "\xCE\x9E"}, {"Yacute", "\xC3\x9D"},
    {"Yuml",   "\xC5\xB8"}, {"Zeta",   "\xCE\x96"}, {"aacute", "\xC3\xA1"},
    {"acirc",  "\xC3\xA2"}, {"acute",  "\xC2\xB4"}, {"aelig",  "\xC3\xA6"},
    {"agrave", "\xC3\xA0"}, {"alefsym","\xE2\x84\xB5"}, {"alpha","\xCE\xB1"},
    {"amp",    "&"},        {"and",    "\xE2\x88\xA7"}, {"ang",  "\xE2\x88\xA0"},
    {"apos",   "'"},        {"aring",  "\xC3\xA5"}, {"asymp",  "\xE2\x89\x88"},
    {"atilde", "\xC3\xA3"}, {"auml",   "\xC3\xA4"}, {"bdquo",  "\xE2\x80\x9E"},
    {"beta",   "\xCE\xB2"}, {"brvbar", "\xC2\xA6"}, {"bull",   "\xE2\x80\xA2"},
    {"cap",    "\xE2\x88\xA9"}, {"ccedil","\xC3\xA7"}, {"cedil", "\xC2\xB8"},
    {"cent",   "\xC2\xA2"}, {"chi",    "\xCE\xC7"}, {"circ",   "\xCB\x86"},
    {"clubs",  "\xE2\x99\xA3"}, {"cong","\xE2\x89\x85"}, {"copy", "\xC2\xA9"},
    {"crarr",  "\xE2\x86\xB5"}, {"cup","\xE2\x88\xAA"}, {"curren","\xC2\xA4"},
    {"dArr",   "\xE2\x87\x93"}, {"dagger","\xE2\x80\xA0"}, {"darr","\xE2\x86\x93"},
    {"deg",    "\xC2\xB0"}, {"delta",  "\xCE\xB4"}, {"diams",  "\xE2\x99\xA6"},
    {"divide", "\xC3\xB7"}, {"eacute", "\xC3\xA9"}, {"ecirc",  "\xC3\xAA"},
    {"egrave", "\xC3\xA8"}, {"empty",  "\xE2\x88\x85"}, {"emsp", "\xE2\x80\x83"},
    {"ensp",   "\xE2\x80\x82"}, {"epsilon","\xCE\xB5"}, {"equiv","\xE2\x89\xA1"},
    {"eta",    "\xCE\xB7"}, {"eth",    "\xC3\xB0"}, {"euml",   "\xC3\xAB"},
    {"euro",   "\xE2\x82\xAC"}, {"exist","\xE2\x88\x83"}, {"fnof","\xC6\x92"},
    {"forall", "\xE2\x88\x80"}, {"frac12","\xC2\xBD"}, {"frac14","\xC2\xBC"},
    {"frac34", "\xC2\xBE"}, {"frasl",  "\xE2\x81\x84"}, {"gamma","\xCE\xB3"},
    {"ge",     "\xE2\x89\xA5"}, {"gt",   ">"},        {"hArr",   "\xE2\x87\x94"},
    {"harr",   "\xE2\x86\x94"}, {"hearts","\xE2\x99\xA5"}, {"hellip","\xE2\x80\xA6"},
    {"iacute", "\xC3\xAD"}, {"icirc",  "\xC3\xAE"}, {"iexcl",  "\xC2\xA1"},
    {"igrave", "\xC3\xAC"}, {"image",  "\xE2\x84\x91"}, {"infin","\xE2\x88\x9E"},
    {"int",    "\xE2\x88\xAB"}, {"iota", "\xCE\xB9"}, {"iquest","\xC2\xBF"},
    {"isin",   "\xE2\x88\x88"}, {"iuml", "\xC3\xAF"}, {"kappa", "\xCE\xBA"},
    {"lArr",   "\xE2\x87\x90"}, {"lambda","\xCE\xBB"}, {"lang", "\xE2\x8C\xA9"},
    {"laquo",  "\xC2\xAB"}, {"larr",   "\xE2\x86\x90"}, {"lceil","\xE2\x8C\x88"},
    {"ldquo",  "\xE2\x80\x9C"}, {"le",   "\xE2\x89\xA4"}, {"lfloor","\xE2\x8C\x8A"},
    {"lowast", "\xE2\x88\x97"}, {"loz",  "\xE2\x97\x8A"}, {"lrm",  "\xE2\x80\x8E"},
    {"lsaquo", "\xE2\x80\xB9"}, {"lsquo","\xE2\x80\x98"}, {"lt",   "<"},
    {"macr",   "\xC2\xAF"}, {"mdash",  "\xE2\x80\x94"}, {"micro","\xC2\xB5"},
    {"middot", "\xC2\xB7"}, {"minus",  "\xE2\x88\x92"}, {"mu",   "\xCE\xBC"},
    {"nabla",  "\xE2\x88\x87"}, {"nbsp", "\xC2\xA0"}, {"ndash", "\xE2\x80\x93"},
    {"ne",     "\xE2\x89\xA0"}, {"ni",   "\xE2\x88\x8B"}, {"not",  "\xC2\xAC"},
    {"notin",  "\xE2\x88\x89"}, {"nsub", "\xE2\x8A\x84"}, {"ntilde","\xC3\xB1"},
    {"nu",     "\xCE\xBD"}, {"oacute", "\xC3\xB3"}, {"ocirc",  "\xC3\xB4"},
    {"oelig",  "\xC5\x93"}, {"ograve", "\xC3\xB2"}, {"oline",  "\xE2\x80\xBE"},
    {"omega",  "\xCF\x89"}, {"omicron","\xCE\xBF"}, {"oplus",  "\xE2\x8A\x95"},
    {"or",     "\xE2\x88\xA8"}, {"ordf", "\xC2\xAA"}, {"ordm",  "\xC2\xBA"},
    {"oslash", "\xC3\xB8"}, {"otilde", "\xC3\xB5"}, {"otimes", "\xE2\x8A\x97"},
    {"ouml",   "\xC3\xB6"}, {"para",   "\xC2\xB6"}, {"part",  "\xE2\x88\x82"},
    {"permil", "\xE2\x80\xB0"}, {"perp", "\xE2\x8A\xA5"}, {"phi",  "\xCF\x86"},
    {"pi",     "\xCF\x80"}, {"piv",    "\xCF\x96"}, {"plusmn", "\xC2\xB1"},
    {"pound",  "\xC2\xA3"}, {"prime",  "\xE2\x80\xB2"}, {"prod", "\xE2\x88\x8F"},
    {"prop",   "\xE2\x88\x9D"}, {"psi",  "\xCF\x88"}, {"quot",  "\""},
    {"rArr",   "\xE2\x87\x92"}, {"radic","\xE2\x88\x9A"}, {"rang", "\xE2\x8C\xAA"},
    {"raquo",  "\xC2\xBB"}, {"rarr",   "\xE2\x86\x92"}, {"rceil","\xE2\x8C\x89"},
    {"rdquo",  "\xE2\x80\x9D"}, {"real", "\xE2\x84\x9C"}, {"reg",  "\xC2\xAE"},
    {"rfloor", "\xE2\x8C\x8B"}, {"rho",  "\xCF\x81"}, {"rlm",   "\xE2\x80\x8F"},
    {"rsaquo", "\xE2\x80\xBA"}, {"rsquo","\xE2\x80\x99"}, {"sbquo","\xE2\x80\x9A"},
    {"scaron", "\xC5\xA1"}, {"sdot",   "\xE2\x8B\x85"}, {"sect", "\xC2\xA7"},
    {"shy",    "\xC2\xAD"}, {"sigma",  "\xCF\x83"}, {"sigmaf","\xCF\x82"},
    {"sim",    "\xE2\x88\xBC"}, {"spades","\xE2\x99\xA0"}, {"sub",  "\xE2\x8A\x82"},
    {"sube",   "\xE2\x8A\x86"}, {"sum",  "\xE2\x88\x91"}, {"sup",  "\xE2\x8A\x83"},
    {"sup1",   "\xC2\xB9"}, {"supe",   "\xE2\x8A\x87"}, {"sup2", "\xC2\xB2"},
    {"sup3",   "\xC2\xB3"}, {"szlig",  "\xC3\x9F"}, {"tau",   "\xCF\x84"},
    {"there4", "\xE2\x88\xB4"}, {"theta","\xCE\xB8"}, {"thetasym","\xCF\x91"},
    {"thinsp", "\xE2\x80\x89"}, {"thorn","\xC3\xBE"}, {"tilde", "\xCB\x9C"},
    {"times",  "\xC3\x97"}, {"trade",  "\xE2\x84\xA2"}, {"uArr", "\xE2\x87\x91"},
    {"uacute", "\xC3\xBA"}, {"uarr",   "\xE2\x86\x91"}, {"ucirc","\xC3\xBB"},
    {"ugrave", "\xC3\xB9"}, {"uml",    "\xC2\xA8"}, {"upsih", "\xCF\x92"},
    {"upsilon","\xCF\x85"}, {"uuml",   "\xC3\xBC"}, {"weierp","\xE2\x84\x98"},
    {"xi",     "\xCE\xBE"}, {"yacute", "\xC3\xBD"}, {"yen",   "\xC2\xA5"},
    {"yuml",   "\xC3\xBF"}, {"zeta",   "\xCE\xB6"}, {"zwj",   "\xE2\x80\x8D"},
    {"zwnj",   "\xE2\x80\x8C"},
};

static const char* lookup_named_entity(std::string_view name) {
    const auto* begin = std::begin(kEntities);
    const auto* end   = std::end(kEntities);
    const auto* it = std::lower_bound(begin, end, name,
        [](const NamedEntity& e, std::string_view n) { return e.name < n; });
    if (it != end && name == it->name) return it->utf8;
    return nullptr;
}

// Windows-1252 mapping for numeric refs 0x80–0x9F (browsers honour this).
static unsigned int cp1252_override(unsigned int cp) noexcept {
    switch (cp) {
        case 0x80: return 0x20AC; case 0x82: return 0x201A; case 0x83: return 0x0192;
        case 0x84: return 0x201E; case 0x85: return 0x2026; case 0x86: return 0x2020;
        case 0x87: return 0x2021; case 0x88: return 0x02C6; case 0x89: return 0x2030;
        case 0x8A: return 0x0160; case 0x8B: return 0x2039; case 0x8C: return 0x0152;
        case 0x8E: return 0x017D; case 0x91: return 0x2018; case 0x92: return 0x2019;
        case 0x93: return 0x201C; case 0x94: return 0x201D; case 0x95: return 0x2022;
        case 0x96: return 0x2013; case 0x97: return 0x2014; case 0x98: return 0x02DC;
        case 0x99: return 0x2122; case 0x9A: return 0x0161; case 0x9B: return 0x203A;
        case 0x9C: return 0x0153; case 0x9E: return 0x017E; case 0x9F: return 0x0178;
        default:   return cp;
    }
}

// Decode one entity at s[i] (s[i] must be '&'). On success appends the
// decoded bytes to `out` and returns the number of source bytes consumed;
// returns 0 when this is not a valid entity (caller emits '&' literally).
static size_t decode_entity(std::string_view s, size_t i, std::string& out) {
    const size_t n = s.size();
    if (i + 1 >= n) return 0;

    // Numeric: &#123; or &#x1F600;
    if (s[i + 1] == '#') {
        size_t j = i + 2;
        bool hex = false;
        if (j < n && (s[j] == 'x' || s[j] == 'X')) { hex = true; ++j; }
        const size_t ds = j;
        unsigned long cp = 0;
        const int base = hex ? 16 : 10;
        const auto dig = [&](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (hex && c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (hex && c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        while (j < n) {
            const int d = dig(s[j]);
            if (d < 0) break;
            cp = cp * static_cast<unsigned long>(base) + static_cast<unsigned long>(d);
            if (cp > 0x11'0000UL) cp = 0x11'0000UL;   // clamp; refuse overflow
            ++j;
        }
        if (j == ds || j >= n || s[j] != ';') return 0;   // no digits / no ';'
        if (cp == 0 || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        else cp = cp1252_override(static_cast<unsigned int>(cp));
        append_utf8(out, static_cast<unsigned int>(cp));
        return j - i + 1;
    }

    // Named: [A-Za-z][A-Za-z0-9]{0,31};  (all table entries fit that shape)
    if ((s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z')) {
        size_t j = i + 2;
        while (j < n && j - i <= 33 &&
               ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') ||
                (s[j] >= '0' && s[j] <= '9')))
            ++j;
        if (j < n && s[j] == ';') {
            const char* u = lookup_named_entity(s.substr(i + 1, j - i - 1));
            if (u) { out += u; return j - i + 1; }
        }
    }
    return 0;
}

// Whole-string decode (attribute values). Fast path: no '&' → unchanged.
static std::string decode_entities(std::string_view s) {
    if (s.find('&') == std::string_view::npos) return std::string(s);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t ate = (s[i] == '&') ? decode_entity(s, i, out) : 0;
        if (ate) { i += ate; continue; }
        out += s[i++];
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Pattern engine — fast matcher for common scrape patterns + regex fallback
// ═══════════════════════════════════════════════════════════════════════════
//
//  Most scraping patterns have one of these shapes:
//      price: ([\d.]+)          literal + bounded group
//      class="([^"]+)"          literal + negated-class group + literal
//      <p>(.*?)</p>             literal + lazy group + literal
//      in-stock                 pure literal ("exists" checks)
//
//  std::regex (libstdc++) pays ~1–2 µs per call in executor setup alone for
//  these. A recognised FastPattern instead runs on memchr/memcmp sweeps —
//  same result, ~10–20× cheaper per match. Everything else falls back to a
//  cached std::regex with identical ECMAScript semantics.

struct FastPattern {
    enum class Kind : uint8_t { NONE, LITERAL, CAPTURE };
    enum class Group : uint8_t { NEGCLASS, LAZY_ANY, GREEDY_ANY };

    Kind  kind  = Kind::NONE;
    Group group = Group::NEGCLASS;
    std::string prefix;          // LITERAL: the literal; CAPTURE: pre-group literal
    std::string suffix;          // CAPTURE: post-group literal (may be empty)
    char    delim = 0;           // NEGCLASS: the excluded character
    int     gmin  = 0;           // NEGCLASS: quantifier bounds
    int     gmax  = 0;
    int     ngroups = 0;         // capture groups (0 LITERAL, 1 CAPTURE)
};

// Is `c` unambiguously a literal in ECMAScript when backslash-escaped?
static bool is_escapable_punct(char c) noexcept {
    // Identity escapes: \X where X is not a letter/digit/underscore.
    const unsigned char u = static_cast<unsigned char>(c);
    return !(std::isalnum(u) || c == '_') && c != 0;
}

// Parse a run of literal characters (identity escapes folded) starting at
// `i`; stops at any unescaped regex metacharacter. Returns chars consumed.
static std::string parse_literal_run(std::string_view pat, size_t& i) {
    std::string out;
    const size_t n = pat.size();
    while (i < n) {
        const char c = pat[i];
        if (c == '\\') {
            if (i + 1 < n && is_escapable_punct(pat[i + 1])) {
                out += pat[i + 1];
                i += 2;
                continue;
            }
            break;                       // \d \w \b ... — not literal
        }
        switch (c) {
            case '(': case ')': case '[': case ']': case '{': case '}':
            case '*': case '+': case '?': case '.': case '|':
            case '^': case '$':
                return out;              // metachar — stop
            default:
                out += c;
                ++i;
        }
    }
    return out;
}

// Parse {m}, {m,}, {m,n} at pat[i]=='{'. Returns false when malformed/absent.
static bool parse_quantifier(std::string_view pat, size_t& i, int& lo, int& hi) {
    const size_t n = pat.size();
    size_t j = i + 1;
    if (j >= n || !std::isdigit(static_cast<unsigned char>(pat[j]))) return false;
    auto read_int = [&](int& out) {
        long v = 0;
        while (j < n && std::isdigit(static_cast<unsigned char>(pat[j]))) {
            v = v * 10 + (pat[j] - '0');
            if (v > 100000) return false;      // sanity bound
            ++j;
        }
        out = static_cast<int>(v);
        return true;
    };
    if (!read_int(lo)) return false;
    if (j < n && pat[j] == '}') { hi = lo; i = j + 1; return true; }
    if (j < n && pat[j] == ',') {
        ++j;
        if (j < n && pat[j] == '}') { hi = INT_MAX; i = j + 1; return true; }
        if (!read_int(hi)) return false;
        if (j < n && pat[j] == '}' && lo <= hi) { i = j + 1; return true; }
    }
    return false;
}

// Recognise the fast shapes; returns Kind::NONE when the pattern needs the
// full regex engine. INV: any pattern mapped to LITERAL/CAPTURE behaves
// byte-identically to std::regex(ECMAScript) for every input.
static FastPattern detect_fast_pattern(std::string_view pat) {
    FastPattern fp;
    const size_t n = pat.size();

    size_t i = 0;
    fp.prefix = parse_literal_run(pat, i);

    if (i == n) {                                   // pure literal
        if (!fp.prefix.empty()) {
            fp.kind = FastPattern::Kind::LITERAL;
            fp.ngroups = 0;
        }
        return fp;
    }

    if (pat[i] != '(') return fp;                   // other meta → fallback
    if (i + 1 < n && pat[i + 1] == '?') return fp;  // (?:...) etc → fallback

    // ── Parse the single capture group ──────────────────────────────────────
    size_t j = i + 1;
    size_t k = 0;                       // one past the group spec (set below)
    if (j + 2 < n && pat[j] == '[' && pat[j + 1] == '^') {
        // [^x] — single excluded char (also accepts the [^]x] corner
        // where ']' is a literal first member, which we treat as excluded).
        k = j + 2;
        char delim;
        if (k < n && pat[k] == '\\' && k + 1 < n && is_escapable_punct(pat[k + 1])) {
            delim = pat[k + 1];
            k += 2;
        } else if (k < n && pat[k] != '[') {
            delim = pat[k];
            ++k;                                   // (']' as first char = literal)
        } else {
            return fp;
        }
        if (k >= n || pat[k] != ']') return fp;    // more than one class atom
        ++k;
        fp.group = FastPattern::Group::NEGCLASS;
        fp.delim = delim;
        fp.gmin = fp.gmax = 1;                     // ( [^x] ) = exactly one char
        if (k < n) {
            if (pat[k] == '*')      { fp.gmin = 0; fp.gmax = INT_MAX; ++k; }
            else if (pat[k] == '+') { fp.gmin = 1; fp.gmax = INT_MAX; ++k; }
            else if (pat[k] == '{') {
                size_t save = k;
                if (!parse_quantifier(pat, k, fp.gmin, fp.gmax)) { (void)save; return fp; }
            }
        }
    } else if (j + 2 < n && pat[j] == '.' && pat[j + 1] == '*' && pat[j + 2] == '?') {
        fp.group = FastPattern::Group::LAZY_ANY;
        fp.gmin = 0; fp.gmax = INT_MAX;
        k = j + 3;
    } else if (j + 1 < n && pat[j] == '.' && pat[j + 1] == '*') {
        fp.group = FastPattern::Group::GREEDY_ANY;
        fp.gmin = 0; fp.gmax = INT_MAX;
        k = j + 2;
    } else {
        return fp;                                  // complex group → fallback
    }

    if (k >= n || pat[k] != ')') return fp;         // group must close here
    i = k + 1;

    // ── Literal suffix (rest of the pattern must be literal) ────────────────
    fp.suffix = parse_literal_run(pat, i);
    if (i != n) return fp;

    if (fp.prefix.empty() && fp.suffix.empty()) return fp;   // no anchor at all
    if (fp.group == FastPattern::Group::LAZY_ANY && fp.suffix.empty()) return fp;
    if (fp.suffix.find('\n') != std::string::npos ||
        fp.suffix.find('\r') != std::string::npos) return fp;

    fp.kind = FastPattern::Kind::CAPTURE;
    fp.ngroups = 1;
    return fp;
}

// Window into the text for one match.
struct MatchView {
    size_t pos, len;              // whole match
    size_t gpos, glen;            // capture group 1 (== whole when no groups)
};

// Anchored fast match at `pos`; false when the pattern fails there.
// INV: `pos + prefix.size() <= n` and prefix verified to occur at pos.
static bool fast_match_at(const FastPattern& fp,
                          const char* d, size_t n, size_t pos,
                          MatchView& m) noexcept {
    if (fp.kind == FastPattern::Kind::LITERAL) {
        m = {pos, fp.prefix.size(), pos, fp.prefix.size()};
        return true;
    }
    const size_t g = pos + fp.prefix.size();
    const size_t sl = fp.suffix.size();

    if (fp.group == FastPattern::Group::NEGCLASS) {
        // Count non-delim chars available, capped at gmax.
        size_t allowed = 0;
        {
            const size_t cap = static_cast<size_t>(fp.gmax) <= n - g
                ? static_cast<size_t>(fp.gmax) : n - g;
            const char* hit = static_cast<const char*>(
                std::memchr(d + g, fp.delim, cap));
            allowed = hit ? static_cast<size_t>(hit - (d + g)) : cap;
        }
        if (allowed < static_cast<size_t>(fp.gmin)) return false;

        if (sl == 0) {
            // Whole trailing region: capture everything up to delim/gmax.
            m = {pos, (g - pos) + allowed, g, allowed};
            return true;
        }
        // Greedy with suffix: pick the largest k in [g+gmin, g+allowed] such
        // that the suffix matches at k (regex backtracking semantics).
        if (n < sl) return false;
        const size_t kmin = g + static_cast<size_t>(fp.gmin);
        size_t k = g + allowed;
        if (k > n - sl) k = n - sl;
        if (k < kmin) return false;
        for (;; --k) {
            if (d[k] == fp.suffix[0] &&
                (sl == 1 || std::memcmp(d + k + 1, fp.suffix.data() + 1, sl - 1) == 0)) {
                m = {pos, (k + sl) - pos, g, k - g};
                return true;
            }
            if (k == kmin) return false;
        }
    }

    // Dot groups never cross line terminators (ECMAScript '.').
    const size_t rest = n - g;
    size_t eol = n;
    if (const char* nl = static_cast<const char*>(std::memchr(d + g, '\n', rest)))
        eol = static_cast<size_t>(nl - d);
    if (const char* cr = static_cast<const char*>(
            std::memchr(d + g, '\r', eol == n ? rest : eol - g)))
        eol = static_cast<size_t>(cr - d);

    if (fp.group == FastPattern::Group::GREEDY_ANY) {
        if (sl == 0) {
            m = {pos, eol - pos, g, eol - g};
            return true;
        }
        // Last suffix occurrence before eol.
        if (n < sl) return false;
        size_t k = eol;
        if (k > n - sl) k = n - sl;
        if (k < g) return false;
        for (;; --k) {
            if (d[k] == fp.suffix[0] &&
                (sl == 1 || std::memcmp(d + k + 1, fp.suffix.data() + 1, sl - 1) == 0)) {
                m = {pos, (k + sl) - pos, g, k - g};
                return true;
            }
            if (k == g) return false;
        }
    }

    // LAZY_ANY: first suffix occurrence before eol (suffix guaranteed non-empty).
    const char* hit = find_bytes(d + g, eol - g, fp.suffix.data(), sl);
    if (!hit) return false;
    const size_t k = static_cast<size_t>(hit - d);
    m = {pos, (k + sl) - pos, g, k - g};
    return true;
}

// Count capturing groups in an ECMAScript pattern: skips escapes, character
// classes, and non-capturing constructs ( (?:, (?=, (?! … ).
static int count_capture_groups(const std::string& pattern) {
    int groups = 0;
    bool escaped = false, in_class = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (escaped)  { escaped = false; continue; }
        if (ch == '\\') { escaped = true;  continue; }
        if (in_class) { if (ch == ']') in_class = false; continue; }
        if (ch == '[') { in_class = true; continue; }
        if (ch == '(') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '?') continue; // (?: (?= (?! …
            ++groups;
        }
    }
    return groups;
}

// The longest run of characters the pattern must match at the START of every
// match ("" when the pattern allows a meta-heavy start). Used to seed
// std::regex searches with memchr+memcmp.
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
        const char nxt = pattern[out.size()];
        if (nxt == '*' || nxt == '?' || nxt == '{')
            out.pop_back();
    }
    return out;
}

namespace {

// Compiled-pattern cache shared across worker threads. Patterns are stable
// for a job, so the hot path is a shared-lock hit. Compile failures are
// cached too and surfaced as config errors instead of being silently
// replaced with a never-matching pattern.
struct CachedPattern {
    FastPattern fast;                     // kind == NONE → use `re`
    std::unique_ptr<std::regex> re;       // compiled only for fallback patterns
    std::string seed;                     // literal-prefix anchor for `re`
    bool valid = false;
};

std::unordered_map<std::string, CachedPattern> g_pattern_cache;
std::shared_mutex g_pattern_cache_mu;

const CachedPattern& get_pattern(const std::string& pattern, const char* context) {
    {
        std::shared_lock lk(g_pattern_cache_mu);
        auto it = g_pattern_cache.find(pattern);
        if (it != g_pattern_cache.end()) {
            if (!it->second.valid)
                throw config_error(std::string("invalid regex in ") + context + ": " + pattern);
            return it->second;
        }
    }
    std::unique_lock lk(g_pattern_cache_mu);
    auto it = g_pattern_cache.find(pattern);
    if (it != g_pattern_cache.end()) {
        if (!it->second.valid)
            throw config_error(std::string("invalid regex in ") + context + ": " + pattern);
        return it->second;
    }

    CachedPattern entry;
    entry.fast = detect_fast_pattern(pattern);
    if (entry.fast.kind != FastPattern::Kind::NONE) {
        // Recognised shape — by construction a valid ECMAScript pattern.
        entry.valid = true;
    } else {
        try {
            entry.re = std::make_unique<std::regex>(
                pattern, std::regex::ECMAScript | std::regex::optimize);
            entry.seed  = literal_prefix(pattern);
            entry.valid = true;
        } catch (const std::regex_error&) {
            entry.valid = false;
        }
    }
    auto res = g_pattern_cache.emplace(pattern, std::move(entry));
    if (!res.first->second.valid)
        throw config_error(std::string("invalid regex in ") + context + ": " + pattern);
    return res.first->second;
}

// regex_sub needs a real std::regex even for fast-shaped patterns. Compiled
// lazily (regex_sub is rare) and cached inside the pattern's own entry.
const std::regex& get_full_regex(const CachedPattern& entry,
                                 const std::string& pattern) {
    if (entry.re) return *entry.re;
    std::unique_lock lk(g_pattern_cache_mu);
    auto it = g_pattern_cache.find(pattern);
    if (it != g_pattern_cache.end() && !it->second.re) {
        try {
            it->second.re = std::make_unique<std::regex>(
                pattern, std::regex::ECMAScript | std::regex::optimize);
        } catch (const std::regex_error&) {
            throw config_error("invalid regex in regex_sub: " + pattern);
        }
        return *it->second.re;
    }
    // Not in cache (or raced a clear): compile a static copy.
    static std::unordered_map<std::string, std::regex> fallback;
    auto fit = fallback.find(pattern);
    if (fit == fallback.end()) {
        try {
            fit = fallback.emplace(
                pattern, std::regex(pattern,
                    std::regex::ECMAScript | std::regex::optimize)).first;
        } catch (const std::regex_error&) {
            throw config_error("invalid regex in regex_sub: " + pattern);
        }
    }
    return fit->second;
}

} // anonymous namespace

// Unified match iteration: cb(whole-pos, whole-len, group-pos, group-len).
// Non-overlapping, leftmost — identical ordering to std::regex_iterator.
template <typename F>
static void scan_pattern(std::string_view text, const CachedPattern& cp, F&& cb) {
    const char* d = text.data();
    const size_t n = text.size();

    if (cp.fast.kind != FastPattern::Kind::NONE) {
        const FastPattern& fp = cp.fast;
        const std::string& anchor = fp.prefix;   // LITERAL: itself
        size_t pos = 0;
        MatchView m{};
        while (pos + anchor.size() <= n) {
            const char* hit = find_bytes(d + pos, n - pos, anchor.data(), anchor.size());
            if (!hit) return;
            const size_t off = static_cast<size_t>(hit - d);
            if (fast_match_at(fp, d, n, off, m)) {
                cb(m.pos, m.len, m.gpos, m.glen);
                pos = m.pos + m.len;             // non-empty by construction
            } else {
                pos = off + 1;
            }
        }
        return;
    }

    // ── std::regex fallback, seeded by the literal prefix when present ──────
    using sv_match_iter = std::regex_iterator<std::string_view::const_iterator>;
    using sv_match      = std::match_results<std::string_view::const_iterator>;

    const std::string& prefix = cp.seed;
    if (prefix.size() < 2) {                      // no useful anchor
        for (sv_match_iter it(text.begin(), text.end(), *cp.re), end; it != end; ++it) {
            const auto& m = *it;
            const size_t wp = static_cast<size_t>(m.position(0));
            const size_t wl = static_cast<size_t>(m.length(0));
            // Group 1 not participating → npos, callers emit "" (regex semantics)
            if (m.size() > 1) {
                cb(wp, wl,
                   m[1].matched ? static_cast<size_t>(m.position(1)) : SIZE_MAX,
                   m[1].matched ? static_cast<size_t>(m.length(1)) : 0);
            } else {
                cb(wp, wl, wp, wl);
            }
        }
        return;
    }

    size_t pos = 0;
    while (pos + prefix.size() <= n) {
        const char* hit = find_bytes(d + pos, n - pos, prefix.data(), prefix.size());
        if (!hit) return;
        const size_t off = static_cast<size_t>(hit - d);
        sv_match m;
        if (std::regex_search(text.begin() + static_cast<ptrdiff_t>(off), text.end(),
                              m, *cp.re, std::regex_constants::match_continuous)) {
            const size_t wp = static_cast<size_t>(m.position(0)) + off;
            const size_t wl = static_cast<size_t>(m.length(0));
            if (m.size() > 1) {
                cb(wp, wl,
                   m[1].matched ? static_cast<size_t>(m.position(1)) + off : SIZE_MAX,
                   m[1].matched ? static_cast<size_t>(m.length(1)) : 0);
            } else {
                cb(wp, wl, wp, wl);
            }
            const size_t mend = wp + wl;
            pos = mend > wp ? mend : wp + 1;     // empty matches advance by one
        } else {
            pos = off + 1;
        }
    }
}

// Number of capture groups, accounting the fast path (always exact).
static int pattern_groups(const std::string& pattern, const CachedPattern& cp) {
    if (cp.fast.kind != FastPattern::Kind::NONE) return cp.fast.ngroups;
    return count_capture_groups(pattern);
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

        if (self_closing || is_void_element(doc.tag_names[id])) {
            emit(id, i, gt, gt + 1, gt + 1);                    // empty content span
        } else {
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
    std::string tag;                    // empty → any tag
    std::string id;
    std::vector<std::string> classes;
    std::vector<AttrSelector> attrs;
    bool empty = false;                 // unparseable / empty selector
};

// Parse one simple-selector segment (`div#a.b[x="y"]`).
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
            // Find the matching ']' honouring quoted values (a quoted value
            // may itself contain ']' or whitespace).
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
            ++i;  // stray combinators/pseudo-classes consumed here if any remain
        }
    }
    return ps;
}

static ParsedSelector parse_selector(const std::string& sel) {
    // For compound selectors keep only the rightmost simple segment —
    // e.g. `div.card > a` behaves like `a`. The scan is bracket- and
    // quote-aware: `a[href="x y"]` must not split on the quoted space.
    size_t seg_start = 0;
    int depth = 0;
    char quote = 0;
    const size_t n = sel.size();
    for (size_t i = 0; i < n; ++i) {
        const char c = sel[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (depth > 0 && (c == '"' || c == '\'')) { quote = c; continue; }
        if (c == '[') { ++depth; continue; }
        if (c == ']') { if (depth > 0) --depth; continue; }
        if (depth == 0 && (is_ws(c) || c == '>' || c == '+' || c == '~'))
            seg_start = i + 1;
    }
    std::string_view seg(sel.data() + seg_start, n - seg_start);
    while (!seg.empty() && (is_ws(seg.back()) || seg.back() == '>' ||
                            seg.back() == '+' || seg.back() == '~'))
        seg.remove_suffix(1);

    ParsedSelector ps;
    if (seg.empty()) { ps.empty = true; return ps; }
    ps = parse_simple_selector(seg);
    if (ps.tag.empty() && ps.id.empty() && ps.classes.empty() && ps.attrs.empty())
        ps.empty = true;
    return ps;
}

// ── Attribute lookup ────────────────────────────────────────────────────────

// Lex one open tag's attribute list, invoking cb(name, value) for each.
// `open_tag` must start at '<'. Values are raw views (entities undecoded,
// quotes stripped); valueless attributes report an empty value. Stop early by
// returning false from cb.
template <typename F>
static void for_each_attr(std::string_view open_tag, F&& cb) {
    if (open_tag.size() < 2 || open_tag[0] != '<') return;
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
                if (i < n) ++i;                                 // closing quote
            } else {
                const size_t vs = i;
                while (i < n && !is_ws(open_tag[i]) && open_tag[i] != '>') ++i;
                value = open_tag.substr(vs, i - vs);
            }
        }
        if (!cb(name, value)) return;
    }
}

// First value of `name_lc` in the tag starting at open_tag[0], or nullopt.
static std::optional<std::string_view> find_attr_value(std::string_view open_tag,
                                                       std::string_view name_lc) {
    std::optional<std::string_view> hit;
    for_each_attr(open_tag, [&](std::string_view name, std::string_view value) {
        if (ci_eq(name, name_lc)) { hit = value; return false; }
        return true;
    });
    return hit;
}

std::vector<ExtractionEngine::Element> ExtractionEngine::find_elements(
        const Document& doc, const std::string& html, const std::string& selector) {
    const ParsedSelector ps = parse_selector(selector);
    std::vector<Element> result;
    if (ps.empty) return result;                   // empty selector → no match

    const bool needs_attrs = !ps.id.empty() || !ps.classes.empty() || !ps.attrs.empty();
    const char* base = html.data();

    for (const auto& el : doc.elements) {
        const std::string& tag = doc.tag_names[el.tag];
        if (!ps.tag.empty() && tag != ps.tag) continue;
        if (!needs_attrs) { result.push_back(el); continue; }

        // Single lex of the open tag covers id + classes + all attr selectors.
        const std::string_view open_tag(base + el.open, el.open_gt - el.open + 1);
        std::string_view id_v, class_v;
        bool has_id = false, has_class = false;
        std::vector<uint8_t> attr_seen(ps.attrs.size(), 0), attr_fail(ps.attrs.size(), 0);
        for_each_attr(open_tag, [&](std::string_view name, std::string_view value) {
            if (!has_id && ci_eq(name, "id")) { id_v = value; has_id = true; return true; }
            if (!has_class && ci_eq(name, "class")) { class_v = value; has_class = true; return true; }
            for (size_t a = 0; a < ps.attrs.size(); ++a) {
                if (!ci_eq(name, ps.attrs[a].name)) continue;
                attr_seen[a] = 1;
                if (ps.attrs[a].has_value && value != ps.attrs[a].value)
                    attr_fail[a] = 1;
            }
            return true;
        });
        bool attrs_ok = true;
        for (size_t a = 0; a < ps.attrs.size(); ++a)
            if (!attr_seen[a] || attr_fail[a]) { attrs_ok = false; break; }
        if (!attrs_ok) continue;
        if (!ps.id.empty() && (!has_id || id_v != ps.id)) continue;
        if (!ps.classes.empty()) {
            if (!has_class) continue;
            bool all = true;
            for (const auto& cls : ps.classes) {
                bool hit = false;
                size_t i = 0;
                while (i < class_v.size()) {
                    while (i < class_v.size() && is_ws(class_v[i])) ++i;
                    const size_t s = i;
                    while (i < class_v.size() && !is_ws(class_v[i])) ++i;
                    if (i > s && class_v.substr(s, i - s) == cls) { hit = true; break; }
                }
                if (!hit) { all = false; break; }
            }
            if (!all) continue;
        }
        result.push_back(el);
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Attribute / text extraction
// ═══════════════════════════════════════════════════════════════════════════

std::optional<std::string> ExtractionEngine::extract_attribute(
        std::string_view element_html, std::string_view attr) {
    const std::string attr_lc = to_lower(attr);
    const size_t n = element_html.size();

    // Fast path: the element's own open tag (covers the overwhelmingly common
    // `a[href]`/self-attribute shape). Fall through to the legacy descendant
    // scan only when the open tag lacks the attribute.
    size_t i = 0;
    while (i + 1 < n) {
        const size_t lt = (i == 0 && element_html[0] == '<')
            ? 0 : element_html.find('<', i);
        if (lt == std::string_view::npos || lt + 1 >= n) break;
        const char c1 = element_html[lt + 1];
        if (c1 == '!' || c1 == '?' || !is_tag_name_start(c1)) {  // skip decls & close tags
            i = lt + 1;
            continue;
        }
        const size_t gt = find_tag_end(element_html, lt + 1);
        if (gt == std::string_view::npos) break;

        if (auto v = find_attr_value(
                element_html.substr(lt, gt - lt + 1),
                std::string_view(attr_lc.data(), attr_lc.size()))) {
            return (v->find('&') == std::string_view::npos)
                ? std::string(*v)                // no entities: copy straight
                : decode_entities(*v);
        }
        i = gt + 1;
    }
    return std::nullopt;
}

std::string ExtractionEngine::extract_text(std::string_view element_html,
                                           bool is_html) {
    // Non-HTML payload (bulk extraction from text/CSV/logs): no tag-stripping,
    // no entity decoding — the matched bytes are the value.
    if (!is_html) return std::string(element_html);

    // Fast path: no markup and no entities → optional edge-trim only, and the
    // common case (clean text) copies the input verbatim with one allocation.
    if (element_html.find('<') == std::string_view::npos &&
        element_html.find('&') == std::string_view::npos) {
        size_t b = 0, e = element_html.size();
        while (b < e && is_ws(element_html[b])) ++b;
        while (e > b && is_ws(element_html[e - 1])) --e;
        bool runs = false;                       // interior whitespace run?
        for (size_t k = b; !runs && k + 1 < e; ++k)
            runs = is_ws(element_html[k]) && is_ws(element_html[k + 1]);
        // runs of identical single spaces are fine; only collapse needed.
        if (!runs) return std::string(element_html.substr(b, e - b));
    }

    // Full path: strip markup, comments, scripts; decode entities; collapse
    // whitespace runs to single spaces (with edge trim for free).
    std::string out;
    out.reserve(element_html.size());
    bool pending_space = false;
    bool at_word_start = true;

    auto push_char = [&](char c) {
        if (is_ws(c)) { pending_space = true; return; }
        if (pending_space && !at_word_start) out += ' ';
        pending_space = false;
        at_word_start = false;
        out += c;
    };
    auto push_str = [&](std::string_view v) { for (char c : v) push_char(c); };

    const char* d = element_html.data();
    const size_t n = element_html.size();
    size_t i = 0;
    while (i < n) {
        const char c = element_html[i];
        if (c == '<' && i + 1 < n) {
            const char c1 = element_html[i + 1];
            if (c1 == '!') {                                     // <!-- --> / <!doctype> / CDATA
                if (element_html.compare(i + 2, 2, "--") == 0) {
                    const size_t e = element_html.find("-->", i + 4);
                    i = (e == std::string_view::npos) ? n : e + 3;
                } else if (element_html.compare(i + 2, 7, "[CDATA[") == 0) {
                    const size_t e = element_html.find("]]>", i + 9);
                    const size_t content_end = (e == std::string_view::npos) ? n : e;
                    for (size_t k = i + 9; k < content_end; ++k) push_char(element_html[k]);
                    i = (e == std::string_view::npos) ? n : e + 3;
                } else {
                    const size_t e = element_html.find('>', i + 2);
                    i = (e == std::string_view::npos) ? n : e + 1;
                }
                continue;
            }
            if (c1 == '?') {
                const size_t e = element_html.find('>', i + 2);
                i = (e == std::string_view::npos) ? n : e + 1;
                continue;
            }
            const bool closing = c1 == '/';
            const size_t name_start = i + 1 + (closing ? 1 : 0);
            if (name_start < n && is_tag_name_start(element_html[name_start])) {
                size_t j = name_start + 1;
                while (j < n && is_tag_name_char(element_html[j])) ++j;
                const size_t gt = find_tag_end(element_html, j);
                if (gt == std::string_view::npos) break;             // unterminated: drop rest
                if (!closing) {
                    const std::string_view tag(d + name_start, j - name_start);
                    if (ci_eq(tag, "script") || ci_eq(tag, "style")) {
                        size_t k = gt;
                        while (k > j && is_ws(element_html[k - 1])) --k;
                        const bool self_closing = k > j && element_html[k - 1] == '/';
                        if (!self_closing) {
                            // Raw-text element: skip to its close tag entirely.
                            const size_t tl = tag.size();
                            size_t cp = std::string_view::npos;
                            for (size_t p = gt + 1; p + 2 + tl <= n;) {
                                const size_t lt = element_html.find('<', p);
                                if (lt == std::string_view::npos || lt + 2 + tl > n) break;
                                if (element_html[lt + 1] == '/' &&
                                    ci_eq(std::string_view(d + lt + 2, tl), tag)) {
                                    cp = lt;
                                    break;
                                }
                                p = lt + 1;
                            }
                            if (cp == std::string_view::npos) { i = n; continue; }
                            const size_t close_gt = find_tag_end(element_html, cp + 2 + tl);
                            i = (close_gt == std::string_view::npos) ? n : close_gt + 1;
                            continue;
                        }
                    }
                }
                i = gt + 1;
                continue;
            }
            // Stray '<' (not a tag): literal text below.
        }
        if (c == '&') {
            std::string decoded;
            const size_t ate = decode_entity(element_html, i, decoded);
            if (ate) {
                push_str(decoded);
                i += ate;
                continue;
            }
        }
        push_char(c);
        ++i;
    }
    return out;   // trailing pending_space simply never materialised
}

// ═══════════════════════════════════════════════════════════════════════════
//  URL join helper
// ═══════════════════════════════════════════════════════════════════════════

static std::string urljoin(const std::string& base, const std::string& rel) {
    if (rel.empty())  return base;
    if (base.empty()) return rel;
    if (rel.find("://") != std::string::npos) return rel;      // absolute
    if (rel.compare(0, 7, "mailto:") == 0 ||
        rel.compare(0, 4, "tel:") == 0 ||
        rel.compare(0, 5, "data:") == 0 ||
        rel.compare(0, 11, "javascript:") == 0) return rel;    // non-hierarchical

    const size_t se = base.find("://");
    if (se == std::string::npos) return rel;                   // base not absolute
    const std::string_view scheme(base.data(), se);
    const size_t ps = base.find('/', se + 3);                  // path start
    const std::string_view host =
        ps == std::string::npos
            ? std::string_view(base).substr(se + 3)
            : std::string_view(base).substr(se + 3, ps - se - 3);

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

    // Strip base query/fragment for the document parts below.
    std::string_view base_path =
        ps == std::string::npos ? std::string_view("/") : std::string_view(base).substr(ps);
    {
        const size_t q = base_path.find_first_of("?#");
        if (q != std::string_view::npos) base_path = base_path.substr(0, q);
    }

    if (rel[0] == '?') {                                       // query-only reference
        std::string out;
        out.reserve(se + 3 + host.size() + base_path.size() + rel.size());
        out.append(scheme).append("://").append(host).append(base_path).append(rel);
        return out;
    }
    if (rel[0] == '#') {                                       // fragment-only
        std::string out;
        out.reserve(se + 3 + host.size() + base_path.size() + rel.size());
        out.append(scheme).append("://").append(host).append(base_path).append(rel);
        return out;
    }

    // Document-relative: resolve against the base directory.
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
//  Transform (values arrive raw; the cleaning sieve runs here, before any
//  transform, so every stage downstream sees the canonical value)
// ═══════════════════════════════════════════════════════════════════════════

// A rule's clean mask overrides the config default; -1 means inherit.
static inline uint32_t resolve_clean_mask(const ExtractRule& rule,
                                          uint32_t cfg_clean) noexcept {
    return rule.clean >= 0 ? static_cast<uint32_t>(rule.clean) : cfg_clean;
}

// Numeric conversions tolerate scraped formatting ("$1,299.50", "-€2,000"):
// parse strictly first, then canonicalise with the numeric cleaner and retry.
static bool tolerant_stoll(const std::string& in, long long& out) {
    try { out = std::stoll(in); return true; } catch (...) {}
    std::string t = in;
    if (!clean_numeric_inplace(t)) return false;
    try { out = std::stoll(t); return true; } catch (...) { return false; }
}
static bool tolerant_stod(const std::string& in, double& out) {
    try { out = std::stod(in); return true; } catch (...) {}
    std::string t = in;
    if (!clean_numeric_inplace(t)) return false;
    try { out = std::stod(t); return true; } catch (...) { return false; }
}

json ExtractionEngine::apply_transforms(
        std::vector<std::string> values,
        const ExtractRule& rule,
        const std::string& base_url,
        uint32_t clean_mask) {
    json out = json::array();

    // A trailing int/float conversion determines the JSON value type.
    const bool to_int   = !rule.transform.empty() && rule.transform.back() == "int";
    const bool to_float = !rule.transform.empty() && rule.transform.back() == "float";

    for (auto& raw : values) {
        std::string v = std::move(raw);

        // ── Cleaning sieve (entities → invisibles → fold → numeric → ws) ──
        if (clean_mask) {
            apply_cleaners(v, clean_mask);
            // "n/a", "-", "" … become JSON null straight away; transforms do
            // not run on null values.
            if ((clean_mask & CLEAN_NULL_TOKEN) && is_null_token(v)) {
                out.push_back(json(nullptr));
                continue;
            }
        }

        for (const auto& t : rule.transform) {
            if (t == "trim") {
                v = trim_str(v);
            } else if (t == "lowercase") {
                for (auto& c : v) c = lc(c);
            } else if (t == "uppercase") {
                for (auto& c : v) c = (c >= 'a' && c <= 'z')
                    ? static_cast<char>(c - 'a' + 'A') : c;
            } else if (t == "urljoin") {
                v = urljoin(base_url, v);
            } else if (t == "int" || t == "float") {
                long long iv; double dv;
                if (t == "int") {
                    v = tolerant_stoll(v, iv) ? std::to_string(iv) : std::string();
                } else {
                    v = tolerant_stod(v, dv) ? std::to_string(dv) : std::string();
                }
            } else if (t == "regex_sub" && rule.regex_sub) {
                const CachedPattern& cp = get_pattern(rule.regex.value_or(""), "regex_sub");
                v = std::regex_replace(v, get_full_regex(cp, rule.regex.value_or("")),
                                       *rule.regex_sub);
            }
            // unknown transforms are ignored on purpose (forward-compatible)
        }
        if (to_int) {
            long long iv;
            out.push_back(tolerant_stoll(v, iv) ? json(iv) : json(nullptr));
        } else if (to_float) {
            double dv;
            out.push_back(tolerant_stod(v, dv) ? json(dv) : json(nullptr));
        } else {
            out.push_back(json(std::move(v)));
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

    if (agg == "count")  return json(static_cast<long long>(raw_count));
    if (agg == "exists") return json(raw_count > 0);

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
        return json(std::move(joined));
    }

    return transformed;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Leaf extraction
// ═══════════════════════════════════════════════════════════════════════════

json ExtractionEngine::extract_leaf(const SubQuery& sub,
                                    std::string_view element_html,
                                    const std::string& base_url,
                                    bool is_html,
                                    uint32_t clean_mask) {
    const auto& rule = sub.rule;
    std::vector<std::string> raw;

    if (rule.text) {
        raw = {extract_text(element_html, is_html)};
    } else if (rule.attribute) {
        if (auto val = extract_attribute(element_html, *rule.attribute))
            raw = {std::move(*val)};
    } else if (rule.regex) {
        const CachedPattern& cp = get_pattern(*rule.regex, "rule");
        const bool want_group = pattern_groups(*rule.regex, cp) >= 1;
        scan_pattern(element_html, cp, [&](size_t wp, size_t wl, size_t gp, size_t gl) {
            if (want_group) {
                raw.emplace_back(gp == SIZE_MAX ? std::string()
                                                : element_html.substr(gp, gl));
            } else {
                raw.emplace_back(element_html.substr(wp, wl));
            }
        });
    }

    if (raw.empty()) {
        if (rule.optional)  return json(nullptr);
        if (rule.aggregate) return apply_aggregate(json::array(), rule, 0);
        return json::array();
    }

    const size_t raw_count = raw.size();
    json transformed = apply_transforms(std::move(raw), rule, base_url, clean_mask);
    return apply_aggregate(transformed, rule, raw_count);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Full query execution
// ═══════════════════════════════════════════════════════════════════════════

std::vector<Record> ExtractionEngine::execute(
        const ScraperConfig& cfg,
        const std::string& html,
        const std::string& base_url,
        bool is_html) {

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
            merged[sub.name] = json(static_cast<long long>(element_count));
            return;
        }
        if (agg == "exists" && no_extraction && rule.transform.empty()) {
            merged[sub.name] = json(element_count > 0);
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
            merged[sub.name] = json(any);
            return;
        }
        merged[sub.name] = apply_aggregate(all_values, rule, all_values.size());
    };

    // Evaluate queries in name order: the config container is unordered and
    // record order would otherwise depend on hashing. Name order is stable
    // across runs and platforms.
    std::vector<const CollectionQuery*> ordered;
    ordered.reserve(cfg.queries.size());
    for (const auto& [qname, query] : cfg.queries) {
        (void)qname;
        ordered.push_back(&query);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const CollectionQuery* a, const CollectionQuery* b) {
                  return a->name < b->name;
              });

    for (const CollectionQuery* qp : ordered) {
        const CollectionQuery& query = *qp;
        if (query.xpath) continue;               // XPath requires a full DOM — unsupported here

        // ── Raw-regex query ────────────────────────────────────────────────
        // Matches are enumerated left to right over the whole payload; each
        // match yields one record containing every extract field.
        if (query.regex) {
            const CachedPattern& cp = get_pattern(*query.regex, "query");
            const bool want_group = pattern_groups(*query.regex, cp) >= 1;
            scan_pattern(html, cp, [&](size_t wp, size_t wl, size_t gp, size_t gl) {
                const size_t tp = want_group ? gp : wp;
                const size_t tl = want_group ? gl : wl;

                Record rec;
                for (const auto& sub : query.extract) {
                    const auto& r = sub.rule;
                    if (r.regex) {
                        // Sub-regex runs inside the matched text.
                        std::string_view seg(html.data() + tp, tl);
                        const CachedPattern& scp = get_pattern(*r.regex, "rule");
                        const bool sgroup = pattern_groups(*r.regex, scp) >= 1;
                        std::vector<std::string> vals;
                        scan_pattern(seg, scp, [&](size_t w2, size_t l2,
                                                   size_t g2, size_t gl2) {
                            if (sgroup) {
                                vals.emplace_back(g2 == SIZE_MAX ? std::string()
                                                                 : seg.substr(g2, gl2));
                            } else {
                                vals.emplace_back(seg.substr(w2, l2));
                            }
                        });
                        if (vals.empty()) {
                            rec[sub.name] = r.optional ? json(nullptr) : json::array();
                        } else {
                            rec[sub.name] = apply_aggregate(
                                apply_transforms(std::move(vals), r, base_url,
                                                 resolve_clean_mask(r, cfg.clean)),
                                r, 0);
                        }
                    } else if (r.text || r.attribute) {
                        rec[sub.name] = extract_leaf(
                            sub, std::string_view(html.data() + tp, tl), base_url,
                            is_html, resolve_clean_mask(r, cfg.clean));
                    } else {
                        json t = apply_transforms(
                            {std::string(html, tp, tl)}, r, base_url,
                            resolve_clean_mask(r, cfg.clean));
                        rec[sub.name] = apply_aggregate(t, r, 1);
                    }
                }
                all_records.push_back(std::move(rec));
            });
            continue;
        }

        // ── Selector-based query (HTML only) ───────────────────────────────
        if (!is_html) continue;                  // selectors need markup
        if (!doc) doc = parse_document(html);
        auto elements = find_elements(*doc, html, query.selector);

        const bool has_aggregate =
            std::any_of(query.extract.begin(), query.extract.end(),
                        [](const SubQuery& s) { return s.rule.aggregate.has_value(); });

        if (has_aggregate && query.multiple) {
            // Cross-element aggregate: per-element extraction, then merge.
            std::vector<Record> per_element;
            per_element.reserve(elements.size());
            const std::string_view hv(html);
            for (const auto& el : elements) {
                const std::string_view span = hv.substr(el.open, el.end - el.open);
                Record rec;
                for (const auto& sub : query.extract)
                    rec[sub.name] = extract_leaf(sub, span, base_url, is_html,
                                                 resolve_clean_mask(sub.rule, cfg.clean));
                per_element.push_back(std::move(rec));
            }

            Record merged;
            for (const auto& sub : query.extract)
                merge_into_record(merged, sub, per_element, elements.size());
            all_records.push_back(std::move(merged));
        } else {
            if (!query.multiple && elements.size() > 1)
                elements.resize(1);
            const std::string_view hv(html);
            for (const auto& el : elements) {
                const std::string_view span = hv.substr(el.open, el.end - el.open);
                Record rec;
                for (const auto& sub : query.extract)
                    rec[sub.name] = extract_leaf(sub, span, base_url, is_html,
                                                 resolve_clean_mask(sub.rule, cfg.clean));
                all_records.push_back(std::move(rec));
            }
        }
    }

    return all_records;
}

json ExtractionEngine::execute_scalar(const CollectionQuery& query,
                                      const std::string& html,
                                      bool is_html) {
    if (query.extract.empty()) return json(nullptr);
    // No config in scope: use the default sieve, honouring per-rule overrides.
    json rec = json::object();
    for (const auto& sub : query.extract)
        rec[sub.name] = extract_leaf(sub, html, "", is_html,
                                     resolve_clean_mask(sub.rule, CLEAN_DEFAULT));
    return rec;
}

} // namespace bullet_scrape
